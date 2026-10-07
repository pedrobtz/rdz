/*
 * rdz_native_r.c -- the native codecs at the R boundary (plan-c Stages E to
 * H): which R objects they take, and the objects they read back.
 *
 * Native: a logical, integer, double or character vector (ALTREP ones from
 * their materialised data); a factor (an integer vector of class "factor"
 * or c("ordered", "factor") with character levels, every code a level or
 * NA); a list; a data frame (a list whose class includes "data.frame",
 * every column as long as it has rows) -- and, as list elements, columns
 * and attribute values, any of these and NULL, to RDZ_MAX_DEPTH levels.
 * Each may carry any attributes whose names are ASCII and whose values are
 * native themselves (Stage I: a Date's class, a POSIXct's tzone, a
 * matrix's dim and dimnames), its own attributes included; the codecs hold
 * names, a factor's levels and class, and a data frame's row.names and
 * class. Not native: S4 objects, row.names off a data frame, and a
 * `.internal.selfref` off a data.table -- the one registered transient
 * attribute, omitted on write (read back as a NULL pointer, as R_Unserialize()
 * gives it, which data.table rebuilds by itself). Anything else
 * anywhere in the object leaves the whole root to the generic codec
 * (AGENTS.md). Strings are bytes plus R's encoding tag; a native-encoded
 * non-ASCII string is not portable (portability.md).
 *
 * The plan is built breadth-first, without recursion, into buffers behind
 * an external pointer; writing and reading run under R_UnwindProtect(), so
 * an interrupt between blocks frees everything at once. R thread only.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>
#include <Rversion.h>

#include <zubin/rw.h>
#include <zufast/utf8.h>

#include "../core/rdz_content.h"
#include "../core/rdz_graph.h"
#include "../core/rdz_numeric.h"
#include "../rdz_r.h"

/* ---- attributes, through the API of each R version -------------------------------- */

/* R 4.6 removed ATTRIB() from the API; R_getAttribCount() and R_mapAttrib()
   replace it. */
static R_xlen_t rdz_attribute_count(SEXP x)
{
#if R_VERSION >= R_Version(4, 6, 0)
    return R_getAttribCount(x);
#else
    R_xlen_t count = 0;
    SEXP node;
    for (node = ATTRIB(x); node != R_NilValue; node = CDR(node)) count++;
    return count;
#endif
}

/* One attribute in R's order. */
typedef struct {
    SEXP tag, value;
} rdz_attr_pair;

typedef struct {
    SEXP names, row_names, cls, levels, selfref;
    int others;
    zb_buf *all; /* rdz_attr_pair, every attribute in order */
    int failed;  /* `all` could not grow */
} rdz_attr_scan;

static void rdz_scan_one(rdz_attr_scan *a, SEXP tag, SEXP value)
{
    rdz_attr_pair pair;
    if (tag == R_NamesSymbol) a->names = value;
    else if (tag == R_RowNamesSymbol) a->row_names = value; /* as stored: compact or not */
    else if (tag == R_ClassSymbol) a->cls = value;
    else if (tag == R_LevelsSymbol) a->levels = value;
    else if (strcmp(CHAR(PRINTNAME(tag)), ".internal.selfref") == 0) a->selfref = value;
    else a->others++;
    pair.tag = tag;
    pair.value = value;
    if (a->all && zb_put_bytes(a->all, &pair, sizeof pair)) a->failed = 1;
}

#if R_VERSION >= R_Version(4, 6, 0)
static SEXP rdz_scan_fun(SEXP tag, SEXP value, void *data)
{
    rdz_scan_one((rdz_attr_scan *)data, tag, value);
    return NULL;
}
#endif

/* x's attributes as stored, without the expansion getAttrib() gives
   compact row names. */
static void rdz_scan_attributes(SEXP x, rdz_attr_scan *a, zb_buf *all)
{
    a->names = a->row_names = a->cls = a->levels = a->selfref = R_NilValue;
    a->others = 0;
    a->all = all;
    a->failed = 0;
    if (all) zb_buf_reset(all);
#if R_VERSION >= R_Version(4, 6, 0)
    R_mapAttrib(x, rdz_scan_fun, a);
#else
    {
        SEXP node;
        for (node = ATTRIB(x); node != R_NilValue; node = CDR(node)) {
            rdz_scan_one(a, TAG(node), CAR(node));
        }
    }
#endif
}

/* A data frame column's rows: a nested data frame's rows (its row names,
   compact or not), anything else's length; UINT64_MAX for a nested data
   frame with malformed row names. */
static int rdz_has_class(SEXP cls, const char *a);

static uint64_t rdz_column_rows(SEXP col)
{
    rdz_attr_scan a;
    SEXP rn;
    if (TYPEOF(col) != VECSXP) return (uint64_t)Rf_xlength(col);
    rdz_scan_attributes(col, &a, NULL);
    if (!rdz_has_class(a.cls, "data.frame")) return (uint64_t)Rf_xlength(col);
    rn = a.row_names;
    if (TYPEOF(rn) == INTSXP && XLENGTH(rn) == 2 && INTEGER_RO(rn)[0] == NA_INTEGER) {
        int m = INTEGER_RO(rn)[1];
        return (uint64_t)(m < 0 ? -(int64_t)m : m);
    }
    if (TYPEOF(rn) == INTSXP || TYPEOF(rn) == STRSXP) return (uint64_t)XLENGTH(rn);
    return UINT64_MAX;
}

/* ---- strings ---------------------------------------------------------------------- */

/* Whether a string is ASCII: R's own flag where its API has it, else a
   scan of the bytes. */
static int rdz_ascii(SEXP c)
{
#if R_VERSION >= R_Version(4, 5, 0)
    return Rf_charIsASCII(c);
#else
    const unsigned char *p = (const unsigned char *)CHAR(c);
    int i, n = LENGTH(c);
    for (i = 0; i < n; i++) {
        if (p[i] & 0x80u) return 0;
    }
    return 1;
#endif
}

static uintptr_t rdz_r_key(void *ctx, size_t i)
{
    return (uintptr_t)STRING_ELT((SEXP)ctx, (R_xlen_t)i);
}

static int rdz_r_value(void *ctx, size_t i, rdz_str *out, rdz_error *e)
{
    SEXP c = STRING_ELT((SEXP)ctx, (R_xlen_t)i);
    if (c == NA_STRING) {
        out->tag = RDZ_STR_NA;
        out->bytes = NULL;
        out->len = 0;
        return 0;
    }
    switch (Rf_getCharCE(c)) {
    case CE_NATIVE:
        if (!rdz_ascii(c)) {
            /* in the session's encoding, which a UTF-8 locale (R's default)
               does not mark: stored as its UTF-8, which every reader can
               mark; R_alloc()ed only when the locale is not UTF-8 */
            const char *u = Rf_translateCharUTF8(c);
            size_t n = strlen(u);
            /* a conversion that loses nothing converts back: in a locale
               that cannot represent the bytes (the C locale), R writes
               escapes such as <e9> instead */
            if (!zuf_utf8_valid(u, n) ||
                (u != CHAR(c) && strcmp(Rf_reEnc(u, CE_UTF8, CE_NATIVE, 0), CHAR(c)) != 0)) {
                return rdz_unsupported(e, "a string that is not valid in the session's encoding");
            }
            if (n + RDZ_STRING_RECORD_HEADER > RDZ_BLOCK_SIZE) {
                return rdz_unsupported(e, "a string larger than one block (1 MiB)");
            }
            out->tag = RDZ_STR_UTF8;
            out->bytes = (const uint8_t *)u;
            out->len = n;
            return 0;
        }
        out->tag = RDZ_STR_NATIVE;
        break;
    case CE_UTF8: out->tag = RDZ_STR_UTF8; break;
    case CE_LATIN1: out->tag = RDZ_STR_LATIN1; break;
    case CE_BYTES: out->tag = RDZ_STR_BYTES; break;
    default: return rdz_unsupported(e, "a name with an unsupported R encoding tag");
    }
    out->len = (size_t)LENGTH(c);
    if (out->len + RDZ_STRING_RECORD_HEADER > RDZ_BLOCK_SIZE) {
        return rdz_unsupported(e, "a string larger than one block (1 MiB)");
    }
    out->bytes = (const uint8_t *)CHAR(c);
    return 0;
}

/* An attribute's name, one ASCII string. */
static uintptr_t rdz_c_key(void *ctx, size_t i)
{
    (void)i;
    return (uintptr_t)ctx;
}

static int rdz_c_value(void *ctx, size_t i, rdz_str *out, rdz_error *e)
{
    (void)i;
    (void)e;
    out->tag = RDZ_STR_NATIVE;
    out->bytes = (const uint8_t *)ctx;
    out->len = strlen((const char *)ctx);
    return 0;
}

/* ---- the plan --------------------------------------------------------------------- */

typedef struct {
    rdz_vec v;
    zb_buf nodes;   /* rdz_node */
    zb_buf objects; /* SEXP per node (R_NilValue for an attribute's name) */
    zb_buf names;   /* const char * per node: an attribute name, or NULL */
    zb_buf depth;   /* uint32_t per node */
    zb_buf attrs;   /* rdz_attribute */
    zb_buf sources; /* rdz_str_source per node, filled after planning */
    zb_buf pairs;   /* scratch: the attributes of the node being visited */
    zb_buf shared;  /* rdz_shared_slot: large vectors met, by address (open addressing) */
    size_t shared_len;
    SEXP ptr; /* the plan's external pointer: its tag keeps values the plan made */
} rdz_plan;

static void rdz_plan_free(rdz_plan *p)
{
    rdz_vec_free(&p->v);
    zb_buf_release(&p->nodes);
    zb_buf_release(&p->objects);
    zb_buf_release(&p->names);
    zb_buf_release(&p->depth);
    zb_buf_release(&p->attrs);
    zb_buf_release(&p->sources);
    zb_buf_release(&p->pairs);
    zb_buf_release(&p->shared);
}

static void rdz_plan_finalize(SEXP ptr)
{
    rdz_plan *p = (rdz_plan *)R_ExternalPtrAddr(ptr);
    if (p) {
        rdz_plan_free(p);
        free(p);
        R_ClearExternalPtr(ptr);
    }
}

static uint32_t rdz_plan_count(const rdz_plan *p)
{
    return (uint32_t)(p->nodes.len / sizeof(rdz_node));
}

static rdz_node *rdz_plan_node(rdz_plan *p, uint32_t i)
{
    return (rdz_node *)(void *)p->nodes.data + i;
}

static SEXP rdz_plan_object(rdz_plan *p, uint32_t i)
{
    return ((SEXP *)(void *)p->objects.data)[i];
}

/* Appends a node; NULL with *why set when the plan cannot grow. */
/* ---- shared objects (Stage O): a large vector met twice is written once ---- */

typedef struct {
    SEXP x;
    uint32_t id;
} rdz_shared_slot;

/* Shared when its data is at least 4 KiB: smaller objects are not worth a
   reference, and R's byte compiler shares small literal constants, which
   would make a value's representation (and hash) depend on compilation. */
static int rdz_shareable(SEXP x)
{
    switch (TYPEOF(x)) {
    case LGLSXP:
    case INTSXP: return XLENGTH(x) >= 1024;
    case REALSXP:
    case STRSXP: return XLENGTH(x) >= 512;
    default: return 0;
    }
}

/* The node id x was planned as, or UINT32_MAX; with `insert`, records it
   as `id` when absent. 0 on success, 1 when the table cannot grow. */
static int rdz_shared_find(rdz_plan *p, SEXP x, uint32_t id, int insert, uint32_t *found)
{
    size_t cap = p->shared.len / sizeof(rdz_shared_slot), at;
    rdz_shared_slot *t;
    *found = UINT32_MAX;
    if (insert && 2 * (p->shared_len + 1) > cap) { /* grow to keep it under half full */
        size_t ncap = cap ? 2 * cap : 64, k;
        zb_buf bigger;
        rdz_shared_slot *old = (rdz_shared_slot *)(void *)p->shared.data, *nt;
        if (zb_buf_alloc(&bigger, 0, 0) || zb_put_zeros(&bigger, ncap * sizeof(rdz_shared_slot))) {
            zb_buf_release(&bigger);
            return 1;
        }
        nt = (rdz_shared_slot *)(void *)bigger.data;
        for (k = 0; k < cap; k++) {
            if (!old[k].x) continue;
            at = ((uintptr_t)old[k].x >> 4) & (ncap - 1);
            while (nt[at].x) at = (at + 1) & (ncap - 1);
            nt[at] = old[k];
        }
        zb_buf_release(&p->shared);
        p->shared = bigger;
        cap = ncap;
    }
    if (!cap) return 0;
    t = (rdz_shared_slot *)(void *)p->shared.data;
    at = ((uintptr_t)x >> 4) & (cap - 1);
    while (t[at].x && t[at].x != x) at = (at + 1) & (cap - 1);
    if (t[at].x) {
        *found = t[at].id;
    } else if (insert) {
        t[at].x = x;
        t[at].id = id;
        p->shared_len++;
    }
    return 0;
}

static rdz_node *rdz_plan_add(rdz_plan *p, SEXP x, const char *name, uint16_t role,
                              uint32_t parent, const char **why)
{
    rdz_node *n;
    SEXP *slot;
    uint32_t d, target = UINT32_MAX, id = rdz_plan_count(p);
    if (rdz_plan_count(p) >= RDZ_MAX_OBJECTS) {
        *why = "an object of more than a million native parts";
        return NULL;
    }
    d = parent == RDZ_ROOT_PARENT_ID ? 0 : ((uint32_t *)(void *)p->depth.data)[parent] + 1;
    if (d > RDZ_MAX_DEPTH) {
        *why = "an object nested more deeply than rdz's native codecs allow";
        return NULL;
    }
    /* x by assignment, not by its address, which rchk would lose track of */
    if (!(slot = (SEXP *)(void *)zb_put_raw(&p->objects, sizeof x)) ||
        zb_put_bytes(&p->names, &name, sizeof name) || zb_put_bytes(&p->depth, &d, sizeof d) ||
        !(n = (rdz_node *)(void *)zb_put_raw(&p->nodes, sizeof *n))) {
        *why = "an object too large to plan";
        return NULL;
    }
    *slot = x;
    memset(n, 0, sizeof *n);
    n->role = role;
    n->parent = parent;
    if (rdz_shareable(x)) {
        if (rdz_shared_find(p, x, id, 1, &target)) {
            *why = "an object too large to plan";
            return NULL;
        }
        if (target != UINT32_MAX) { /* met before: a reference to it */
            n->type = RDZ_TYPE_REFERENCE;
            n->first_child = target;
        }
    }
    return n;
}

/* Adds an attribute (its name and value nodes) to node `owner`. */
static int rdz_plan_attribute(rdz_plan *p, uint32_t owner, const char *name, SEXP value,
                              uint32_t flags, const char **why)
{
    rdz_attribute a;
    rdz_node *n;
    uint32_t id = rdz_plan_count(p);
    if (flags != RDZ_ATTRIBUTE_FLAG_OTHER && rdz_attribute_count(value) != 0) {
        *why = "names, row names or a class with attributes of their own";
        return 1;
    }
    if (!rdz_plan_add(p, R_NilValue, name, RDZ_ROLE_ATTRIBUTE_NAME, owner, why)) return 1;
    n = rdz_plan_node(p, id);
    n->type = RDZ_TYPE_CHARACTER;
    n->length = 1;
    if (!rdz_plan_add(p, value, NULL, RDZ_ROLE_ATTRIBUTE_VALUE, owner, why)) return 1;
    memset(&a, 0, sizeof a);
    a.owner_id = owner;
    a.name_object_id = id;
    a.value_object_id = id + 1;
    a.ordinal = rdz_plan_node(p, owner)->attribute_count++;
    a.flags = flags;
    if (rdz_plan_node(p, owner)->attribute_count == 1) {
        rdz_plan_node(p, owner)->first_attribute = (uint32_t)(p->attrs.len / sizeof a);
    }
    if (zb_put_bytes(&p->attrs, &a, sizeof a)) {
        *why = "an object too large to plan";
        return 1;
    }
    return 0;
}

static int rdz_is_class(SEXP cls, const char *a, const char *b)
{
    if (TYPEOF(cls) != STRSXP || XLENGTH(cls) != (b ? 2 : 1)) return 0;
    if (strcmp(CHAR(STRING_ELT(cls, 0)), a) != 0) return 0;
    return !b || strcmp(CHAR(STRING_ELT(cls, 1)), b) == 0;
}

static int rdz_has_class(SEXP cls, const char *a)
{
    R_xlen_t i;
    if (TYPEOF(cls) != STRSXP) return 0;
    for (i = 0; i < XLENGTH(cls); i++) {
        if (STRING_ELT(cls, i) != NA_STRING && strcmp(CHAR(STRING_ELT(cls, i)), a) == 0) return 1;
    }
    return 0;
}

/* What a node's own codec already holds, so is not a general attribute. */
enum {
    RDZ_SKIP_NAMES = 1, RDZ_SKIP_ROW_NAMES = 2, RDZ_SKIP_CLASS = 4, RDZ_SKIP_LEVELS = 8,
    RDZ_SKIP_SELFREF = 16
};

/* Plans node i's other attributes, in R's order, as general attributes:
   each an ASCII name and any native value, attributes of its own included. */
static const char *rdz_plan_others(rdz_plan *p, uint32_t i, int skip)
{
    size_t k, count = p->pairs.len / sizeof(rdz_attr_pair);
    const char *why = NULL;
    for (k = 0; k < count; k++) {
        rdz_attr_pair pr = ((const rdz_attr_pair *)(const void *)p->pairs.data)[k];
        const char *name = CHAR(PRINTNAME(pr.tag));
        const unsigned char *c;
        if ((pr.tag == R_NamesSymbol && (skip & RDZ_SKIP_NAMES)) ||
            (pr.tag == R_RowNamesSymbol && (skip & RDZ_SKIP_ROW_NAMES)) ||
            (pr.tag == R_ClassSymbol && (skip & RDZ_SKIP_CLASS)) ||
            (pr.tag == R_LevelsSymbol && (skip & RDZ_SKIP_LEVELS)) ||
            ((skip & RDZ_SKIP_SELFREF) && strcmp(name, ".internal.selfref") == 0)) {
            continue;
        }
        if (!*name) return "an attribute with an empty name";
        for (c = (const unsigned char *)name; *c; c++) {
            if (*c & 0x80u) return "an attribute with a non-ASCII name";
        }
        if (rdz_plan_attribute(p, i, name, pr.value, RDZ_ATTRIBUTE_FLAG_OTHER, &why)) return why;
    }
    return NULL;
}

/* Fills node i from its object, appending its levels, attributes and
   children; the reason it cannot be written natively, or NULL. */
static const char *rdz_plan_visit(rdz_plan *p, uint32_t i)
{
    SEXP x = rdz_plan_object(p, i);
    rdz_node *n = rdz_plan_node(p, i);
    int type = TYPEOF(x);
    rdz_attr_scan a;
    const char *why = NULL;

    if (n->role == RDZ_ROLE_ATTRIBUTE_NAME) return NULL; /* filled when added */
    if (n->type == RDZ_TYPE_REFERENCE) return NULL;       /* filled when added */
    if (type == NILSXP) {
        if (i == 0) return "NULL";
        n->type = RDZ_TYPE_NULL;
        return NULL;
    }
    if (type != LGLSXP && type != INTSXP && type != REALSXP && type != STRSXP && type != VECSXP) {
        return Rf_type2char((SEXPTYPE)type);
    }
    /* an ALTREP vector (a compact sequence, a deferred string, a memory
       map) is written from its data, which INTEGER_RO() and the others
       materialise here, on the R thread, before any worker starts */
    /* setting attributes back would not restore the S4 bit */
    if (Rf_isS4(x)) return "an S4 object";
    n->length = (uint64_t)XLENGTH(x);
    rdz_scan_attributes(x, &a, &p->pairs);
    if (a.failed) return "an object too large to plan";
    if (a.selfref != R_NilValue &&
        !(type == VECSXP && rdz_has_class(a.cls, "data.table") &&
          rdz_has_class(a.cls, "data.frame"))) {
        return "an object with a .internal.selfref attribute that is not a data.table";
    }

    /* a factor: levels and class are the codec's */
    if (type == INTSXP && rdz_has_class(a.cls, "factor")) {
        SEXP levels = a.levels;
        const int *codes;
        int ordered = rdz_is_class(a.cls, "ordered", "factor");
        if (!ordered && !rdz_is_class(a.cls, "factor", NULL)) {
            return "a factor whose class is not \"factor\" or c(\"ordered\", \"factor\")";
        }
        if (a.names != R_NilValue || a.row_names != R_NilValue || TYPEOF(levels) != STRSXP ||
            rdz_attribute_count(levels) != 0) {
            return "a factor with names or malformed levels";
        }
        codes = INTEGER_RO(x); /* checked against the levels as they are encoded */
        n->type = RDZ_TYPE_FACTOR;
        n->flags = ordered ? RDZ_OBJECT_FLAG_ORDERED : 0;
        n->values = codes;
        n->first_child = rdz_plan_count(p);
        n->child_count = 1;
        if (!rdz_plan_add(p, levels, NULL, RDZ_ROLE_LEVELS, i, &why)) return why;
        return rdz_plan_others(p, i, RDZ_SKIP_CLASS | RDZ_SKIP_LEVELS);
    }

    /* a data frame: names, row.names and class are the codec's */
    if (type == VECSXP && rdz_has_class(a.cls, "data.frame")) {
        R_xlen_t k, ncol = XLENGTH(x);
        uint64_t nrow;
        int compact;
        if (a.names == R_NilValue || TYPEOF(a.names) != STRSXP || XLENGTH(a.names) != ncol) {
            return "a data frame with malformed names";
        }
        /* compact row names are c(NA_integer_, -n), automatic, or
           c(NA_integer_, n), the stored row names 1:n (as from df[1:n, ]):
           those are written as 1:n, which R makes compact again */
        compact = TYPEOF(a.row_names) == INTSXP && XLENGTH(a.row_names) == 2 &&
                  INTEGER_RO(a.row_names)[0] == NA_INTEGER;
        if (compact) {
            int m = INTEGER_RO(a.row_names)[1];
            nrow = (uint64_t)(m < 0 ? -(int64_t)m : m);
            if (m > 0) {
                SEXP seq = PROTECT(Rf_allocVector(INTSXP, m));
                int *v = INTEGER(seq), j;
                for (j = 0; j < m; j++) v[j] = j + 1;
                R_SetExternalPtrTag(p->ptr, Rf_cons(seq, R_ExternalPtrTag(p->ptr)));
                UNPROTECT(1);
                a.row_names = seq;
                compact = 0;
            }
        } else if (TYPEOF(a.row_names) == INTSXP || TYPEOF(a.row_names) == STRSXP) {
            nrow = (uint64_t)XLENGTH(a.row_names);
        } else {
            return "a data frame with malformed row names";
        }
        for (k = 0; k < ncol; k++) {
            uint64_t rows = rdz_column_rows(VECTOR_ELT(x, k));
            if (rows == UINT64_MAX) return "a data frame column that is a data frame with malformed row names";
            if (rows != nrow) return "a data frame whose columns differ in length";
        }
        n->type = RDZ_TYPE_DATA_FRAME;
        n->length = nrow;
        if (rdz_plan_attribute(p, i, "names", a.names, RDZ_ATTRIBUTE_FLAG_NAMES, &why)) return why;
        if (!compact &&
            rdz_plan_attribute(p, i, "row.names", a.row_names, RDZ_ATTRIBUTE_FLAG_ROW_NAMES, &why)) {
            return why;
        }
        if (!rdz_is_class(a.cls, "data.frame", NULL) &&
            rdz_plan_attribute(p, i, "class", a.cls, RDZ_ATTRIBUTE_FLAG_CLASS, &why)) {
            return why;
        }
        if ((why = rdz_plan_others(p, i, RDZ_SKIP_NAMES | RDZ_SKIP_ROW_NAMES | RDZ_SKIP_CLASS |
                                             RDZ_SKIP_SELFREF)) != NULL) {
            return why;
        }
        n = rdz_plan_node(p, i);
        n->first_child = rdz_plan_count(p);
        n->child_count = (uint32_t)ncol;
        for (k = 0; k < ncol; k++) {
            if (!rdz_plan_add(p, VECTOR_ELT(x, k), NULL, RDZ_ROLE_CHILD, i, &why)) return why;
        }
        return NULL;
    }

    /* anything else: names are the codec's, every other attribute general */
    if (rdz_has_class(a.cls, "factor") || rdz_has_class(a.cls, "data.frame")) {
        return "a factor or data frame of the wrong type";
    }
    if (a.row_names != R_NilValue) return "a vector with row names";
    if (a.names != R_NilValue) {
        if (TYPEOF(a.names) != STRSXP || XLENGTH(a.names) != XLENGTH(x)) {
            return type == LGLSXP ? "a malformed logical vector" : "a malformed vector";
        }
    }
    switch (type) {
    case LGLSXP:
        n->type = RDZ_TYPE_LOGICAL;
        n->values = LOGICAL_RO(x);
        break;
    case INTSXP:
        n->type = RDZ_TYPE_INTEGER;
        n->values = INTEGER_RO(x);
        break;
    case REALSXP:
        n->type = RDZ_TYPE_DOUBLE;
        n->values = REAL_RO(x);
        break;
    case STRSXP:
        n->type = RDZ_TYPE_CHARACTER;
        break;
    default:
        n->type = RDZ_TYPE_LIST;
        break;
    }
    if (a.names != R_NilValue &&
        rdz_plan_attribute(p, i, "names", a.names, RDZ_ATTRIBUTE_FLAG_NAMES, &why)) {
        return why;
    }
    if ((why = rdz_plan_others(p, i, RDZ_SKIP_NAMES)) != NULL) return why;
    if (type == VECSXP) {
        R_xlen_t k, len = XLENGTH(x);
        n = rdz_plan_node(p, i);
        n->first_child = rdz_plan_count(p);
        n->child_count = (uint32_t)len;
        for (k = 0; k < len; k++) {
            if (!rdz_plan_add(p, VECTOR_ELT(x, k), NULL, RDZ_ROLE_CHILD, i, &why)) return why;
        }
    }
    return NULL;
}

/* Plans x breadth-first; the reason it is not native, or NULL. */
static const char *rdz_plan_build(rdz_plan *p, SEXP x)
{
    const char *why = NULL;
    uint32_t i;
    rdz_str_source *src;
    if (!rdz_plan_add(p, x, NULL, RDZ_ROLE_ROOT, RDZ_ROOT_PARENT_ID, &why)) return why;
    for (i = 0; i < rdz_plan_count(p); i++) {
        if ((why = rdz_plan_visit(p, i)) != NULL) return why;
    }
    /* the string sources, now that the node array no longer moves */
    if (zb_put_zeros(&p->sources, (size_t)rdz_plan_count(p) * sizeof(rdz_str_source))) {
        return "an object too large to plan";
    }
    src = (rdz_str_source *)(void *)p->sources.data;
    for (i = 0; i < rdz_plan_count(p); i++) {
        rdz_node *n = rdz_plan_node(p, i);
        const char *name = ((const char **)(void *)p->names.data)[i];
        if (n->type != RDZ_TYPE_CHARACTER) continue;
        if (name) {
            src[i].n = 1;
            src[i].ctx = (void *)(uintptr_t)name;
            src[i].key = rdz_c_key;
            src[i].value = rdz_c_value;
        } else {
            src[i].n = (size_t)n->length;
            src[i].ctx = rdz_plan_object(p, i);
            src[i].key = rdz_r_key;
            src[i].value = rdz_r_value;
        }
        n->strings = &src[i];
    }
    return NULL;
}

/* Many small parts: every object with data costs a directory entry, a block
   entry and header (160 bytes) and a compression frame of its own, so a list
   of 500,000 short vectors is 88 MB native against 1 MB generic. Automatic
   mode leaves an object of at least this many parts averaging less than
   this many bytes of data to the generic codec (a writer policy). */
#define RDZ_SMALL_PARTS_MIN   1024u
#define RDZ_SMALL_PARTS_BYTES 1024u

static int rdz_plan_small_parts(rdz_plan *p)
{
    uint32_t i, parts = 0, count = rdz_plan_count(p);
    double bytes = 0;
    if (count < RDZ_SMALL_PARTS_MIN) return 0;
    for (i = 0; i < count; i++) {
        const rdz_node *n = rdz_plan_node(p, i);
        switch (n->type) {
        case RDZ_TYPE_LOGICAL:
        case RDZ_TYPE_INTEGER:
        case RDZ_TYPE_FACTOR: bytes += 4.0 * (double)n->length; parts++; break;
        case RDZ_TYPE_DOUBLE: bytes += 8.0 * (double)n->length; parts++; break;
        case RDZ_TYPE_CHARACTER: bytes += 8.0 * (double)n->length; parts++; break;
        default: break;
        }
    }
    return parts >= RDZ_SMALL_PARTS_MIN && bytes < (double)RDZ_SMALL_PARTS_BYTES * parts;
}

/* ---- the content hash (container-format.md, "Content hash") -------------------------- */

/* A string vector into the content hash: each element's record digest, in
   order. An R character vector's elements are read straight from its data,
   and a CHARSXP seen before (R's string cache makes equal strings one
   CHARSXP) is neither checked nor digested again. */
#define RDZ_STRING_CACHE 65536u /* direct-mapped, by CHARSXP address */

/* One cache for every string vector of a hash: its keys are CHARSXPs of the
   value being hashed, alive until the .Call returns. */
typedef struct rdz_string_cache {
    SEXP *key;
    uint64_t *digest;
} rdz_string_cache;

static int rdz_hash_strings(rdz_content *c, const rdz_str_source *src, rdz_string_cache *cache,
                            rdz_error *e)
{
    size_t k;
    if (src->value == rdz_r_value) {
        SEXP x = (SEXP)src->ctx;
        const SEXP *elt = STRING_PTR_RO(x);
        if (!cache->key) { /* R_alloc(): reclaimed when the .Call returns */
            cache->key = (SEXP *)(void *)R_alloc(RDZ_STRING_CACHE, sizeof(SEXP));
            cache->digest = (uint64_t *)(void *)R_alloc(RDZ_STRING_CACHE, sizeof(uint64_t));
            memset(cache->key, 0, RDZ_STRING_CACHE * sizeof(SEXP));
        }
        for (k = 0; k < src->n; k++) {
            SEXP ch = elt[k];
            size_t slot = ((uintptr_t)ch >> 4) % RDZ_STRING_CACHE;
            if (cache->key[slot] != ch) {
                rdz_str s;
                if (rdz_r_value(x, k, &s, e)) return 1; /* checks it is native */
                cache->key[slot] = ch;
                cache->digest[slot] = rdz_string_digest(&s);
            }
            rdz_content_digest(c, cache->digest[slot]);
        }
        return 0;
    }
    for (k = 0; k < src->n; k++) {
        rdz_str s;
        if (src->value(src->ctx, k, &s, e)) return 1;
        rdz_content_string(c, &s);
    }
    return 0;
}

/* The native content hash of a plan without writing it (rdz_hash()): the
   stream rdz_graph_write() feeds while writing (rdz_content.h). */
static int rdz_plan_hash(rdz_plan *p, uint8_t out[16], rdz_error *e)
{
    rdz_content content, *c = &content; /* on the stack: the hasher needs its 64-byte alignment */
    rdz_string_cache cache = {NULL, NULL};
    uint32_t i, count = rdz_plan_count(p);
    rdz_content_begin(c);
    for (i = 0; i < count; i++) {
        const rdz_node *n = rdz_plan_node(p, i);
        rdz_content_node(c, n);
        switch (n->type) {
        case RDZ_TYPE_FACTOR: {
            /* the writer refuses codes outside the levels as it encodes, so
               a hash without writing checks them too: a value the writer
               would send generic hashes as it */
            const rdz_node *l = rdz_plan_node(p, n->first_child);
            if (l->type == RDZ_TYPE_REFERENCE) l = rdz_plan_node(p, l->first_child);
            if (!rdz_int_codes_ok((const int32_t *)n->values, (size_t)n->length, l->length)) {
                return rdz_unsupported(e, "a factor with codes outside its levels");
            }
        }
            /* fall through */
        case RDZ_TYPE_LOGICAL:
        case RDZ_TYPE_INTEGER: rdz_content_values(c, n->values, (size_t)n->length, 4); break;
        case RDZ_TYPE_DOUBLE: rdz_content_values(c, n->values, (size_t)n->length, 8); break;
        case RDZ_TYPE_CHARACTER:
            if (rdz_hash_strings(c, n->strings, &cache, e)) return 1;
            break;
        default: break;
        }
    }
    rdz_content_attributes(c, (const rdz_attribute *)(const void *)p->attrs.data,
                           p->attrs.len / sizeof(rdz_attribute));
    rdz_content_end(c, out);
    return 0;
}

/* The canonical XXH128 text of a stored digest: high, then low, in hex. */
SEXP rdz_hash_text(const uint8_t digest[16])
{
    char s[33];
    snprintf(s, sizeof s, "%016llx%016llx", (unsigned long long)zb_rd_u64le(digest + 8),
             (unsigned long long)zb_rd_u64le(digest));
    return Rf_mkString(s);
}

static void rdz_tick(void *ctx)
{
    (void)ctx;
    R_CheckUserInterrupt();
}

typedef struct {
    rdz_plan *p;
    const char *path;
    int policy, level, threads;
    rdz_error e;
    int failed;
} rdz_write_call;

static SEXP rdz_write_body(void *data)
{
    rdz_write_call *c = (rdz_write_call *)data;
    rdz_plan *p = c->p;
    c->failed = rdz_graph_write(&p->v, c->path, (const rdz_node *)(const void *)p->nodes.data,
                                rdz_plan_count(p),
                                (const rdz_attribute *)(const void *)p->attrs.data,
                                (uint32_t)(p->attrs.len / sizeof(rdz_attribute)), c->policy,
                                c->level, c->threads, rdz_tick, NULL, &c->e);
    return R_NilValue;
}

static void rdz_plan_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_plan_finalize((SEXP)data);
}

/* TRUE when written; FALSE when x is not for the native codecs and strict is
   FALSE (automatic mode then writes it generically); a failure otherwise.
   settings: c(level, threads, block size), as rdz_settings() gives them. */
/* A plan of x behind an external pointer (protect it): planned, or the
   reason x is not native (automatic mode's policies included unless
   strict); *out the plan. */
static SEXP rdz_plan_start(SEXP x, int strict, rdz_plan **out, const char **why)
{
    SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, x));
    rdz_plan *p;
    R_RegisterCFinalizerEx(ptr, rdz_plan_finalize, TRUE);
    p = (rdz_plan *)calloc(1, sizeof(rdz_plan));
    if (!p) Rf_error("rdz could not allocate memory for a writer");
    rdz_vec_init(&p->v);
    zb_buf_alloc(&p->nodes, 0, 0);
    zb_buf_alloc(&p->objects, 0, 0);
    zb_buf_alloc(&p->names, 0, 0);
    zb_buf_alloc(&p->depth, 0, 0);
    zb_buf_alloc(&p->attrs, 0, 0);
    zb_buf_alloc(&p->sources, 0, 0);
    zb_buf_alloc(&p->pairs, 0, 0);
    zb_buf_alloc(&p->shared, 0, 0);
    R_SetExternalPtrAddr(ptr, p);
    p->ptr = ptr;
    *out = p;
    *why = rdz_plan_build(p, x);
    if (!*why && !strict && rdz_plan_small_parts(p)) *why = "an object of many small parts";
    UNPROTECT(1);
    return ptr;
}

/* The native content hash of x as rdz would write it: its text, FALSE when
   automatic mode would write it generically, or (strict) an unsupported
   failure. */
SEXP rdz_c_hash_native(SEXP x, SEXP strict)
{
    rdz_plan *p;
    const char *why;
    rdz_error e;
    uint8_t digest[16];
    int s = Rf_asLogical(strict);
    SEXP ptr = PROTECT(rdz_plan_start(x, s, &p, &why));
    if (!why && rdz_plan_hash(p, digest, &e)) {
        /* a string the native codecs cannot take (e says which) */
        rdz_plan_finalize(ptr);
        UNPROTECT(1);
        if (e.code == RDZ_E_UNSUPPORTED && !s) return Rf_ScalarLogical(0);
        return rdz_failure(&e);
    }
    rdz_plan_finalize(ptr);
    UNPROTECT(1);
    if (why) {
        if (!s) return Rf_ScalarLogical(0);
        rdz_unsupported(&e, why);
        return rdz_failure(&e);
    }
    return rdz_hash_text(digest);
}

SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy, SEXP settings,
                            SEXP metadata)
{
    /* no path: into memory, returned as a raw vector (rdz_serialize()) */
    const char *p = path == R_NilValue ? NULL : rdz_path(path);
    SEXP ptr, cont;
    rdz_write_call call;
    const char *why;
    if (TYPEOF(settings) != INTSXP || XLENGTH(settings) < 3 || XLENGTH(settings) > 4) {
        Rf_error("`settings` must be an integer vector of length 3 or 4.");
    }
    ptr = PROTECT(rdz_plan_start(x, Rf_asLogical(strict), &call.p, &why));
    cont = PROTECT(R_MakeUnwindCont());
    /* the writer computes the content hash as it goes, unless asked not to */
    call.p->v.hash_content = XLENGTH(settings) == 4 ? INTEGER(settings)[3] != 0 : 1;
    if (TYPEOF(metadata) == RAWSXP) { /* held by the caller for the whole call */
        call.p->v.metadata = RAW(metadata);
        call.p->v.metadata_len = (uint32_t)XLENGTH(metadata);
    }
    if (why) {
        rdz_plan_finalize(ptr);
        UNPROTECT(2);
        if (!Rf_asLogical(strict)) return Rf_ScalarLogical(0);
        rdz_unsupported(&call.e, why);
        return rdz_failure(&call.e);
    }
    call.path = p;
    call.policy = Rf_asInteger(policy);
    call.level = INTEGER(settings)[0];
    call.threads = INTEGER(settings)[1] < 1 ? 1 : INTEGER(settings)[1];
    call.failed = 0;
    R_UnwindProtect(rdz_write_body, &call, rdz_plan_cleanup, ptr, cont);
    if (!call.failed && !p) {
        /* the bytes, copied before the plan (which owns them) goes */
        const zb_buf *b = &call.p->v.w.result;
        SEXP out = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t)b->len));
        if (b->len) memcpy(RAW(out), b->data, b->len);
        rdz_plan_finalize(ptr);
        UNPROTECT(3);
        return out;
    }
    rdz_plan_finalize(ptr);
    UNPROTECT(2);
    if (!call.failed) return Rf_ScalarLogical(1);
    if (call.e.code == RDZ_E_UNSUPPORTED && !Rf_asLogical(strict)) return Rf_ScalarLogical(0);
    return rdz_failure(&call.e);
}

/* ---- reading ---------------------------------------------------------------------- */

typedef struct {
    SEXP target, dictionary;
    R_xlen_t filled, entries, length, dictionary_length;
    rdz_names_sink sink;
} rdz_r_names;

static cetype_t rdz_r_ce(uint8_t tag)
{
    switch (tag) {
    case RDZ_STR_UTF8: return CE_UTF8;
    case RDZ_STR_LATIN1: return CE_LATIN1;
    case RDZ_STR_BYTES: return CE_BYTES;
    default: return CE_NATIVE;
    }
}

static int rdz_r_make(SEXP into, R_xlen_t *at, R_xlen_t cap, const rdz_str *v, size_t count,
                      rdz_error *e)
{
    size_t i;
    if ((R_xlen_t)count > cap - *at) return rdz_invalid(e, "character object length mismatch");
    for (i = 0; i < count; i++) {
        SEXP c;
        if (v[i].tag == RDZ_STR_NA) {
            c = NA_STRING;
        } else {
            if (v[i].len > INT_MAX) return rdz_invalid(e, "a string is longer than R permits");
            if (v[i].len && memchr(v[i].bytes, 0, v[i].len)) {
                return rdz_invalid(e, "a string holds a NUL byte, which R strings cannot");
            }
            c = Rf_mkCharLenCE((const char *)v[i].bytes, (int)v[i].len, rdz_r_ce(v[i].tag));
        }
        SET_STRING_ELT(into, *at + (R_xlen_t)i, c);
    }
    *at += (R_xlen_t)count;
    return 0;
}

static int rdz_r_plain(void *ctx, const rdz_str *v, size_t count, rdz_error *e)
{
    rdz_r_names *s = (rdz_r_names *)ctx;
    return rdz_r_make(s->target, &s->filled, s->length, v, count, e);
}

static int rdz_r_entries(void *ctx, const rdz_str *v, size_t count, rdz_error *e)
{
    rdz_r_names *s = (rdz_r_names *)ctx;
    return rdz_r_make(s->dictionary, &s->entries, s->dictionary_length, v, count, e);
}

static int rdz_r_indices(void *ctx, const uint32_t *ids, size_t count, rdz_error *e)
{
    rdz_r_names *s = (rdz_r_names *)ctx;
    size_t i;
    if ((R_xlen_t)count > s->length - s->filled) {
        return rdz_invalid(e, "character object length mismatch");
    }
    /* the core rejects an id at or past the entries delivered so far */
    for (i = 0; i < count; i++) {
        SET_STRING_ELT(s->target, s->filled + (R_xlen_t)i,
                       STRING_ELT(s->dictionary, (R_xlen_t)ids[i]));
    }
    s->filled += (R_xlen_t)count;
    return 0;
}

/* Entries in an object's dictionary blocks; validation bounds the total by
   the object's length. */
static size_t rdz_dictionary_length(const rdz_reader *r, uint32_t object)
{
    const rdz_object *o = &r->objects[object];
    uint32_t i;
    uint64_t entries = 0;
    for (i = o->first_block; i < o->first_block + o->block_count; i++) {
        if (r->blocks[i].encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) {
            entries += r->blocks[i].logical_count;
        }
    }
    return (size_t)entries;
}

typedef struct {
    rdz_reader r;
    rdz_vec v;
    SEXP holder;  /* VECSXP: per object its R value, then per object its
                     dictionary (protected through the external pointer) */
    zb_buf sinks; /* rdz_r_names per object */
    SEXP select;  /* R_NilValue, or the root's children to read (0-based
                     integers; held by the caller) */
    SEXP targets; /* R_NilValue, or objects to read alone (0-based ids; held
                     by the caller): the result is the list of them */
    zb_buf want;  /* with a selection: a byte per object, set to read it */
    SEXP window;  /* R_NilValue, or c(lo, hi): read the root's rows (a data frame's
                     vector columns and row names) or elements [lo, hi) */
    zb_buf windows; /* rdz_window per object */
    int threads;
    rdz_error e;
    int failed;
} rdz_graph_in;

/* Marks, from object `from` on, everything below a marked object: its
   children, levels and attribute objects, which all come after it. */
static void rdz_mark_below(const rdz_reader *r, uint8_t *want, uint32_t from)
{
    uint32_t i, c, a;
    int again = 1;
    /* a wanted reference wants its (earlier) target and what is below that:
       repeat until nothing new is marked */
    while (again) {
        again = 0;
        for (i = from; i < r->nobjects; i++) {
            const rdz_object *o = &r->objects[i];
            if (!want[i]) continue;
            if (o->type_tag == RDZ_TYPE_REFERENCE) {
                if (!want[o->first_child]) {
                    want[o->first_child] = 1;
                    again = 1;
                }
                continue;
            }
            for (c = 0; c < o->child_count; c++) want[o->first_child + c] = 1;
            for (a = 0; a < o->attribute_count; a++) {
                const rdz_attribute *at = &r->attributes[o->first_attribute + a];
                want[at->name_object_id] = 1;
                want[at->value_object_id] = 1;
            }
        }
    }
}

/* With a selection, marks what to read: the root; its names, row names and
   class (a selection keeps no other root attribute, which may describe the
   unselected children: a data.table's key); the selected children; and
   everything below a marked object (its children, levels and attributes,
   which all come after it). */
static int rdz_select_mask(rdz_graph_in *g)
{
    const rdz_reader *r = &g->r;
    const rdz_object *root = &r->objects[0];
    const int *sel = INTEGER(g->select);
    R_xlen_t j, k = XLENGTH(g->select);
    uint32_t a;
    uint8_t *want;
    if (root->type_tag != RDZ_TYPE_LIST && root->type_tag != RDZ_TYPE_DATA_FRAME) {
        return rdz_unsupported(&g->e, "selecting from a root that is not a list or a data frame");
    }
    if (zb_put_zeros(&g->want, r->nobjects)) return rdz_memory(&g->e, "the selection");
    want = g->want.data;
    want[0] = 1;
    for (a = 0; a < root->attribute_count; a++) {
        const rdz_attribute *at = &r->attributes[root->first_attribute + a];
        if (at->flags == RDZ_ATTRIBUTE_FLAG_OTHER) continue;
        want[at->name_object_id] = 1;
        want[at->value_object_id] = 1;
    }
    for (j = 0; j < k; j++) {
        if (sel[j] == NA_INTEGER || sel[j] < 0 || (uint32_t)sel[j] >= root->child_count) {
            return rdz_limit(&g->e, "selection index");
        }
        want[root->first_child + (uint32_t)sel[j]] = 1;
    }
    rdz_mark_below(r, want, 1); /* below the selected children, not the root */
    return 0;
}

/* With targets: each target and everything below it. */
static int rdz_target_mask(rdz_graph_in *g)
{
    const rdz_reader *r = &g->r;
    const int *id = INTEGER(g->targets);
    R_xlen_t j, k = XLENGTH(g->targets);
    if (zb_put_zeros(&g->want, r->nobjects)) return rdz_memory(&g->e, "the selection");
    for (j = 0; j < k; j++) {
        if (id[j] == NA_INTEGER || id[j] < 0 || (uint32_t)id[j] >= r->nobjects) {
            return rdz_limit(&g->e, "object id");
        }
        g->want.data[id[j]] = 1;
    }
    rdz_mark_below(r, g->want.data, 0);
    return 0;
}

static void *rdz_r_values(void *ctx, uint32_t object)
{
    rdz_graph_in *g = (rdz_graph_in *)ctx;
    SEXP x = VECTOR_ELT(g->holder, object);
    return TYPEOF(x) == REALSXP   ? (void *)REAL(x)
           : TYPEOF(x) == LGLSXP ? (void *)LOGICAL(x)
                                 : (void *)INTEGER(x);
}

static const rdz_names_sink *rdz_r_sink(void *ctx, uint32_t object)
{
    rdz_graph_in *g = (rdz_graph_in *)ctx;
    return &((rdz_r_names *)(void *)g->sinks.data)[object].sink;
}

/* Rf_setAttrib() for an attribute read from a file, which R may refuse
   (a dim that does not fit, a class R checks): a format error, not R's. */
typedef struct {
    SEXP x, sym, value;
} rdz_set_attr;

static SEXP rdz_set_attr_body(void *data)
{
    rdz_set_attr *s = (rdz_set_attr *)data;
    Rf_setAttrib(s->x, s->sym, s->value);
    return R_NilValue;
}

static SEXP rdz_set_attr_refused(SEXP cond, void *data)
{
    (void)cond;
    *(int *)data = 1;
    return R_NilValue;
}

/* The attribute name object's one string as a symbol name: ASCII, not
   empty, at most the 10,000 bytes R allows a symbol (what writers write).
   NULL when it is not. */
static const char *rdz_attr_name(SEXP nm)
{
    SEXP c = STRING_ELT(nm, 0);
    if (c == NA_STRING || LENGTH(c) == 0 || LENGTH(c) > 10000 || !rdz_ascii(c)) return NULL;
    return CHAR(c);
}

static int rdz_is_data_table(SEXP x)
{
    return rdz_has_class(Rf_getAttrib(x, R_ClassSymbol), "data.table");
}

static int rdz_windowable(const rdz_object *o)
{
    switch (o->type_tag) {
    case RDZ_TYPE_LOGICAL:
    case RDZ_TYPE_INTEGER:
    case RDZ_TYPE_DOUBLE:
    case RDZ_TYPE_CHARACTER:
    case RDZ_TYPE_FACTOR: return 1;
    default: return 0;
    }
}

static void rdz_window_object(const rdz_reader *r, rdz_window *w, uint8_t *slot, uint32_t id,
                              uint64_t lo, uint64_t hi)
{
    const rdz_object *o = &r->objects[id];
    slot[id] = 1;
    if (o->type_tag == RDZ_TYPE_REFERENCE) { /* a shared column: its target, read once */
        id = o->first_child;
        o = &r->objects[id];
    }
    if (!rdz_windowable(o)) return;
    w[id].lo = lo;
    w[id].hi = hi;
    w[id].on = 1;
}

static int rdz_read_attr_name(rdz_reader *r, const rdz_attribute *a, char *out, size_t cap,
                              rdz_error *e);

/* With a window: the root's rows. A data frame's vector columns and its
   stored row names, or a vector and its names; other columns (lists, data
   frames) are read whole, for R to subset. Fills `windows` (r->nobjects of
   them) from the open reader, which it leaves open: rdz_native_read_request()
   runs it first, to read the value whole when it refuses. */
static int rdz_window_fill(rdz_reader *r, double lo, double hi, zb_buf *windows, rdz_error *e)
{
    const rdz_object *root = &r->objects[0];
    uint64_t rows = root->logical_len; /* a data frame's rows, a vector's elements */
    rdz_window *w;
    uint8_t *slot; /* the objects windowed in place: the root's columns, or the root */
    uint32_t k, a, j;
    if (root->type_tag == RDZ_TYPE_LIST || root->type_tag == RDZ_TYPE_NULL ||
        root->type_tag == RDZ_TYPE_REFERENCE) {
        return rdz_unsupported(e, "rows of a root that is not a data frame or a vector");
    }
    if (!(lo >= 0 && hi >= lo && hi <= (double)rows)) return rdz_limit(e, "row range");
    if (zb_put_zeros(windows, (size_t)r->nobjects * sizeof(rdz_window))) {
        return rdz_memory(e, "the row range");
    }
    slot = (uint8_t *)R_alloc(r->nobjects, 1);
    memset(slot, 0, r->nobjects);
    w = (rdz_window *)(void *)windows->data;
    if (root->type_tag == RDZ_TYPE_DATA_FRAME) {
        for (k = 0; k < root->child_count; k++) {
            rdz_window_object(r, w, slot, root->first_child + k, (uint64_t)lo, (uint64_t)hi);
        }
    } else {
        rdz_window_object(r, w, slot, 0, (uint64_t)lo, (uint64_t)hi);
    }
    for (a = 0; a < root->attribute_count; a++) { /* names of a vector, row names of a frame */
        const rdz_attribute *at = &r->attributes[root->first_attribute + a];
        if ((root->type_tag != RDZ_TYPE_DATA_FRAME && at->flags == RDZ_ATTRIBUTE_FLAG_NAMES) ||
            at->flags == RDZ_ATTRIBUTE_FLAG_ROW_NAMES) {
            rdz_window_object(r, w, slot, at->value_object_id, (uint64_t)lo, (uint64_t)hi);
        }
    }
    /* a windowed vector shared with a part read whole would reach that part
       cut short: a reference outside the window to a windowed target, or a
       windowed target that lives outside the window (an attribute, a list
       element, planned before the column that refers to it). Refused, so R
       reads the value whole. */
    for (j = 0; j < r->nobjects; j++) {
        const rdz_object *o = &r->objects[j];
        if ((o->type_tag == RDZ_TYPE_REFERENCE && !slot[j] && w[o->first_child].on) ||
            (w[j].on && !slot[j])) {
            return rdz_unsupported(e, "rows of a vector shared with a part read whole");
        }
        /* a matrix's or a time series' rows are not its elements' */
        if (w[j].on) {
            for (a = 0; a < o->attribute_count; a++) {
                const rdz_attribute *at = &r->attributes[o->first_attribute + a];
                char name[10001]; /* R caps a symbol at 10,000 bytes */
                if (at->flags != RDZ_ATTRIBUTE_FLAG_OTHER) continue;
                if (rdz_read_attr_name(r, at, name, sizeof name, e)) return 1;
                if (!strcmp(name, "dim") || !strcmp(name, "dimnames") || !strcmp(name, "tsp")) {
                    return rdz_unsupported(e, "rows of a matrix, an array or a time series");
                }
            }
        }
    }
    return 0;
}

static int rdz_window_plan(rdz_graph_in *g)
{
    return rdz_window_fill(&g->r, REAL(g->window)[0], REAL(g->window)[1], &g->windows, &g->e);
}

/* Builds the whole value of an open native file. */
static SEXP rdz_graph_body(void *data)
{
    rdz_graph_in *g = (rdz_graph_in *)data;
    rdz_reader *r = &g->r;
    uint32_t i, n = r->nobjects, k;
    rdz_graph_sinks sinks;
    rdz_r_names *names;
    const uint8_t *want = NULL;
    const rdz_window *windows;
    int selecting = g->select != R_NilValue, targeting = g->targets != R_NilValue;
    g->failed = 1;
    if (selecting) {
        if (rdz_select_mask(g)) return R_NilValue;
        want = g->want.data;
    } else if (targeting) {
        if (rdz_target_mask(g)) return R_NilValue;
        want = g->want.data;
    }
    if (g->window != R_NilValue && rdz_window_plan(g)) return R_NilValue;
    windows = g->window != R_NilValue ? (const rdz_window *)(const void *)g->windows.data : NULL;
    for (i = 0; i < n; i++) {
        const rdz_object *o = &r->objects[i];
        SEXP x;
        if (want && !want[i]) continue;
        if (o->logical_len > (uint64_t)R_XLEN_T_MAX || o->child_count > (uint32_t)INT_MAX) {
            rdz_limit(&g->e, "allocation size");
            return R_NilValue;
        }
        if (i == 0 && selecting) {
            SET_VECTOR_ELT(g->holder, 0, Rf_allocVector(VECSXP, XLENGTH(g->select)));
            continue;
        }
        if (o->type_tag == RDZ_TYPE_REFERENCE) { /* validation: an earlier, non-reference object */
            SET_VECTOR_ELT(g->holder, i, VECTOR_ELT(g->holder, o->first_child));
            continue;
        }
        {
            /* a windowed object holds its window */
            R_xlen_t len = windows && windows[i].on ? (R_xlen_t)(windows[i].hi - windows[i].lo)
                                                    : (R_xlen_t)o->logical_len;
            switch (o->type_tag) {
            case RDZ_TYPE_LOGICAL: x = Rf_allocVector(LGLSXP, len); break;
            case RDZ_TYPE_INTEGER:
            case RDZ_TYPE_FACTOR: x = Rf_allocVector(INTSXP, len); break;
            case RDZ_TYPE_DOUBLE: x = Rf_allocVector(REALSXP, len); break;
            case RDZ_TYPE_CHARACTER: x = Rf_allocVector(STRSXP, len); break;
            case RDZ_TYPE_LIST:
            case RDZ_TYPE_DATA_FRAME: x = Rf_allocVector(VECSXP, (R_xlen_t)o->child_count); break;
            default: x = R_NilValue; break;
            }
        }
        SET_VECTOR_ELT(g->holder, i, x);
        if (o->type_tag == RDZ_TYPE_CHARACTER) {
            rdz_r_names *s = &((rdz_r_names *)(void *)g->sinks.data)[i];
            SEXP dict;
            PROTECT(x); /* held by g->holder too, which rchk cannot see */
            dict = Rf_allocVector(STRSXP, (R_xlen_t)rdz_dictionary_length(r, i));
            UNPROTECT(1);
            SET_VECTOR_ELT(g->holder, (R_xlen_t)n + i, dict);
            s->target = x;
            s->dictionary = dict;
            s->filled = 0;
            s->entries = 0;
            s->length = XLENGTH(x);
            s->dictionary_length = XLENGTH(dict);
            s->sink.ctx = s;
            s->sink.plain = rdz_r_plain;
            s->sink.entries = rdz_r_entries;
            s->sink.indices = rdz_r_indices;
        }
    }
    sinks.ctx = g;
    sinks.values = rdz_r_values;
    sinks.strings = rdz_r_sink;
    if (rdz_graph_read_window(&g->v, r, &sinks, want, windows, g->threads, rdz_tick, NULL,
                              &g->e)) {
        return R_NilValue;
    }

    /* last to first: every child, level and attribute value comes after
       its owner, so each is complete before it is attached */
    names = (rdz_r_names *)(void *)g->sinks.data;
    for (i = n; i-- > 0;) {
        const rdz_object *o = &r->objects[i];
        SEXP x = VECTOR_ELT(g->holder, i);
        int have_rn = 0, have_class = 0, root_selection = i == 0 && selecting;
        if (want && !want[i]) continue;
        if (o->type_tag == RDZ_TYPE_REFERENCE) continue; /* its target is assembled in its turn */
        if (o->type_tag == RDZ_TYPE_CHARACTER && names[i].filled != names[i].length) {
            rdz_invalid(&g->e, "character object length mismatch");
            return R_NilValue;
        }
        if (root_selection) {
            const int *sel = INTEGER(g->select);
            R_xlen_t j;
            for (j = 0; j < XLENGTH(g->select); j++) {
                SET_VECTOR_ELT(x, j, VECTOR_ELT(g->holder, o->first_child + (uint32_t)sel[j]));
            }
        } else if (o->type_tag == RDZ_TYPE_LIST || o->type_tag == RDZ_TYPE_DATA_FRAME) {
            for (k = 0; k < o->child_count; k++) {
                SET_VECTOR_ELT(x, (R_xlen_t)k, VECTOR_ELT(g->holder, o->first_child + k));
            }
        }
        if (o->type_tag == RDZ_TYPE_FACTOR) {
            /* the core checked every code against the levels as it decoded */
            SEXP levels = VECTOR_ELT(g->holder, o->first_child), cls;
            if (o->flags & RDZ_OBJECT_FLAG_ORDERED) {
                cls = PROTECT(Rf_allocVector(STRSXP, 2));
                SET_STRING_ELT(cls, 0, Rf_mkChar("ordered"));
                SET_STRING_ELT(cls, 1, Rf_mkChar("factor"));
            } else {
                cls = PROTECT(Rf_mkString("factor"));
            }
            Rf_setAttrib(x, R_LevelsSymbol, levels);
            Rf_setAttrib(x, R_ClassSymbol, cls);
            UNPROTECT(1);
        }
        for (k = 0; k < o->attribute_count; k++) {
            const rdz_attribute *a = &r->attributes[o->first_attribute + k];
            SEXP nm, value;
            const char *name;
            rdz_set_attr set;
            int refused = 0;
            if (root_selection && a->flags == RDZ_ATTRIBUTE_FLAG_OTHER) continue; /* not read */
            nm = VECTOR_ELT(g->holder, a->name_object_id);
            value = VECTOR_ELT(g->holder, a->value_object_id);
            name = rdz_attr_name(nm);
            if (!name) {
                rdz_invalid(&g->e, "an attribute's name is not a plain ASCII name");
                return R_NilValue;
            }
            if (a->flags == RDZ_ATTRIBUTE_FLAG_OTHER) {
                /* never one the codecs hold, so never twice */
                int factor = o->type_tag == RDZ_TYPE_FACTOR,
                    frame = o->type_tag == RDZ_TYPE_DATA_FRAME;
                if (strcmp(name, "names") == 0 || strcmp(name, "row.names") == 0 ||
                    ((factor || frame) && strcmp(name, "class") == 0) ||
                    (factor && strcmp(name, "levels") == 0)) {
                    rdz_invalid(&g->e, "a general attribute the codec holds");
                    return R_NilValue;
                }
            } else {
                const char *want = a->flags == RDZ_ATTRIBUTE_FLAG_NAMES       ? "names"
                                   : a->flags == RDZ_ATTRIBUTE_FLAG_ROW_NAMES ? "row.names"
                                                                              : "class";
                if (strcmp(name, want) != 0) {
                    rdz_invalid(&g->e, "an attribute's name does not match its kind");
                    return R_NilValue;
                }
                if (a->flags == RDZ_ATTRIBUTE_FLAG_ROW_NAMES) have_rn = 1;
                if (a->flags == RDZ_ATTRIBUTE_FLAG_CLASS) have_class = 1;
            }
            if (root_selection && a->flags == RDZ_ATTRIBUTE_FLAG_NAMES) {
                /* the selected children's names, in the selection's order */
                const int *sel = INTEGER(g->select);
                R_xlen_t j, m = XLENGTH(g->select);
                SEXP sub = Rf_allocVector(STRSXP, m);
                SET_VECTOR_ELT(g->holder, a->value_object_id, sub); /* keeps it */
                for (j = 0; j < m; j++) SET_STRING_ELT(sub, j, STRING_ELT(value, sel[j]));
                value = sub;
            }
            set.x = x;
            set.sym = Rf_install(name);
            set.value = value;
            R_tryCatchError(rdz_set_attr_body, &set, rdz_set_attr_refused, &refused);
            if (refused) {
                rdz_invalid(&g->e, "R refuses an attribute's value");
                return R_NilValue;
            }
        }
        if (o->type_tag == RDZ_TYPE_DATA_FRAME) {
            if (!have_rn) {
                SEXP rn = PROTECT(Rf_allocVector(INTSXP, 2));
                INTEGER(rn)[0] = NA_INTEGER;
                /* a frame read over a window has its rows */
                INTEGER(rn)[1] = i == 0 && windows
                                     ? -(int)(REAL(g->window)[1] - REAL(g->window)[0])
                                     : -(int)o->logical_len;
                Rf_setAttrib(x, R_RowNamesSymbol, rn);
                UNPROTECT(1);
            }
            if (!have_class) {
                SEXP cls = PROTECT(Rf_mkString("data.frame"));
                Rf_setAttrib(x, R_ClassSymbol, cls);
                UNPROTECT(1);
            }
            /* a data.table's .internal.selfref (a pointer to the table) is
               not stored: it comes back as R_Unserialize() gives it, a NULL
               pointer, which data.table takes as "loaded from disk" and
               rebuilds by itself at the first change, as after readRDS() */
            if (have_class && rdz_is_data_table(x)) {
                SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
                Rf_setAttrib(x, Rf_install(".internal.selfref"), ptr);
                UNPROTECT(1);
            }
        }
    }
    g->failed = 0;
    if (targeting) {
        /* no allocation follows: the caller protects the list */
        SEXP out = Rf_allocVector(VECSXP, XLENGTH(g->targets));
        R_xlen_t j;
        for (j = 0; j < XLENGTH(g->targets); j++) {
            SET_VECTOR_ELT(out, j, VECTOR_ELT(g->holder, INTEGER(g->targets)[j]));
        }
        return out;
    }
    return VECTOR_ELT(g->holder, 0);
}

static void rdz_graph_in_finalize(SEXP ptr)
{
    rdz_graph_in *g = (rdz_graph_in *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_vec_free(&g->v);
        rdz_reader_close(&g->r);
        zb_buf_release(&g->sinks);
        zb_buf_release(&g->want);
        zb_buf_release(&g->windows);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

static void rdz_graph_in_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_graph_in_finalize((SEXP)data);
}

/* The value of a native file, given its open reader, which this takes over
   (its buffers and file) and closes; with `select` (0-based integers, or
   R_NilValue for all), only those children of a list or data frame root. */
static SEXP rdz_native_read_in(rdz_reader *opened, int threads, SEXP select, SEXP targets,
                               SEXP window, rdz_error *e, int *failed);

static SEXP rdz_native_read_in(rdz_reader *opened, int threads, SEXP select, SEXP targets,
                               SEXP window, rdz_error *e, int *failed)
{
    SEXP ptr, cont, out, holder;
    rdz_graph_in *g;
    *failed = 1;
    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ptr, rdz_graph_in_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    g = (rdz_graph_in *)calloc(1, sizeof *g);
    if (!g) {
        rdz_reader_close(opened);
        Rf_error("rdz could not allocate memory for a reader");
    }
    g->r = *opened;
    rdz_reader_init(opened);
    rdz_vec_init(&g->v);
    g->select = select;
    g->targets = targets;
    g->window = window;
    g->threads = threads;
    R_SetExternalPtrAddr(ptr, g);
    holder = Rf_allocVector(VECSXP, 2 * (R_xlen_t)g->r.nobjects);
    R_SetExternalPtrProtected(ptr, holder);
    g->holder = holder;
    if (zb_buf_alloc(&g->want, 0, 0) || zb_buf_alloc(&g->windows, 0, 0) ||
        zb_buf_alloc(&g->sinks, 0, 0) ||
        zb_put_zeros(&g->sinks, (size_t)g->r.nobjects * sizeof(rdz_r_names))) {
        rdz_memory(e, "the reader");
        rdz_graph_in_finalize(ptr);
        UNPROTECT(2);
        return R_NilValue;
    }
    out = PROTECT(R_UnwindProtect(rdz_graph_body, g, rdz_graph_in_cleanup, ptr, cont));
    *e = g->e;
    *failed = g->failed;
    rdz_graph_in_finalize(ptr);
    UNPROTECT(3);
    return *failed ? R_NilValue : out;
}

/* ---- read_rdz(): select and rows resolved against the open file ------------------ */

/* The root's names, from the open reader alone (it stays open): their
   strings, or R_NilValue when the root has none; *failed on a read error. */
static SEXP rdz_root_names(rdz_reader *r, rdz_error *e, int *failed)
{
    const rdz_object *root = &r->objects[0];
    uint32_t a, object = 0;
    int found = 0;
    rdz_r_names s;
    SEXP out;
    *failed = 0;
    for (a = 0; a < root->attribute_count && !found; a++) {
        const rdz_attribute *at = &r->attributes[root->first_attribute + a];
        if (at->flags == RDZ_ATTRIBUTE_FLAG_NAMES) {
            object = at->value_object_id;
            found = 1;
        }
    }
    if (!found) return R_NilValue;
    /* a value shared with an earlier object: that object's strings */
    if (r->objects[object].type_tag == RDZ_TYPE_REFERENCE) object = r->objects[object].first_child;
    if (r->objects[object].type_tag != RDZ_TYPE_CHARACTER ||
        r->objects[object].logical_len > (uint64_t)R_XLEN_T_MAX) {
        rdz_invalid(e, "names that are not strings");
        *failed = 1;
        return R_NilValue;
    }
    out = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)r->objects[object].logical_len));
    s.dictionary = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)rdz_dictionary_length(r, object)));
    s.target = out;
    s.filled = 0;
    s.entries = 0;
    s.length = XLENGTH(out);
    s.dictionary_length = XLENGTH(s.dictionary);
    s.sink.ctx = &s;
    s.sink.plain = rdz_r_plain;
    s.sink.entries = rdz_r_entries;
    s.sink.indices = rdz_r_indices;
    if (rdz_graph_read_strings(r, object, &s.sink, e)) {
        *failed = 1;
    } else if (s.filled != s.length) {
        rdz_invalid(e, "character object length mismatch");
        *failed = 1;
    }
    UNPROTECT(2);
    return *failed ? R_NilValue : out;
}

/* A request failure (rdz_request()), its data protected meanwhile. */
static SEXP rdz_req(const char *what, SEXP data)
{
    SEXP out;
    PROTECT(data);
    out = rdz_request(what, data);
    UNPROTECT(1);
    return out;
}

/* select (a character or double vector, or FALSE for neither) as the
   root's 0-based children, in R's order of checks: the root, the type,
   then for positions their range, for names NA, the names' presence and
   each name (match(): the first match; NA never matches), and last a part
   named twice. *request is set to a request failure instead. */
static SEXP rdz_select_children(rdz_reader *r, SEXP select, rdz_error *e, int *failed,
                                SEXP *request)
{
    const rdz_object *root = &r->objects[0];
    uint32_t n = root->child_count;
    R_xlen_t j, k = XLENGTH(select), unknown = 0;
    SEXP at = R_NilValue, names, m, missing;
    int *ids, nprot = 0;
    uint8_t *seen;
    *failed = 0;
    *request = R_NilValue;
    if (root->type_tag != RDZ_TYPE_LIST && root->type_tag != RDZ_TYPE_DATA_FRAME) {
        *request = rdz_req("select_root", Rf_mkString(rdz_root_type_name(r)));
        return R_NilValue;
    }
    if (TYPEOF(select) != STRSXP && TYPEOF(select) != REALSXP) {
        *request = rdz_req("select_type", R_NilValue);
        return R_NilValue;
    }
    at = PROTECT(Rf_allocVector(INTSXP, k));
    nprot++;
    ids = INTEGER(at);
    if (TYPEOF(select) == REALSXP) {
        for (j = 0; j < k; j++) {
            double x = REAL(select)[j];
            if (!R_FINITE(x) || x != floor(x) || x < 1 || x > (double)n || x > INT_MAX) {
                *request = rdz_req("select_range", Rf_ScalarReal((double)n));
                goto done;
            }
            ids[j] = (int)x - 1;
        }
    } else {
        names = PROTECT(rdz_root_names(r, e, failed));
        nprot++;
        if (*failed) goto done;
        for (j = 0; j < k; j++) {
            if (STRING_ELT(select, j) == NA_STRING) {
                *request = rdz_req("select_na", R_NilValue);
                goto done;
            }
        }
        if (names == R_NilValue) {
            *request = rdz_req("select_no_names", R_NilValue);
            goto done;
        }
        m = PROTECT(Rf_match(names, select, 0));
        nprot++;
        for (j = 0; j < k; j++) unknown += INTEGER(m)[j] == 0;
        if (unknown) {
            R_xlen_t u = 0;
            missing = PROTECT(Rf_allocVector(STRSXP, unknown));
            nprot++;
            for (j = 0; j < k; j++) {
                if (INTEGER(m)[j] == 0) SET_STRING_ELT(missing, u++, STRING_ELT(select, j));
            }
            *request = rdz_req("select_unknown", missing);
            goto done;
        }
        for (j = 0; j < k; j++) ids[j] = INTEGER(m)[j] - 1;
    }
    seen = (uint8_t *)R_alloc(n ? n : 1, 1);
    memset(seen, 0, n ? n : 1);
    for (j = 0; j < k; j++) {
        if (seen[ids[j]]) {
            *request = rdz_req("select_repeat", R_NilValue);
            goto done;
        }
        seen[ids[j]] = 1;
    }
done:
    UNPROTECT(nprot);
    return *failed || *request != R_NilValue ? R_NilValue : at;
}

/* read_rdz()'s one read of an open native file: select resolved to the
   root's children and the rows' window checked against the root, both from
   this reader, then the value read with it (which takes the reader over).
   The window's refusals for R's words come first: a root without rows,
   then rows past its length; a window the plan refuses (a vector shared
   with a part read whole; a matrix, an array, a time series) reads the
   value whole instead, *lo 0. *failed: 0, 1 (e), or 2 (the value is a
   request failure). */
SEXP rdz_native_read_request(rdz_reader *opened, int threads, SEXP select, SEXP window,
                             rdz_error *e, int *failed, double *lo)
{
    const rdz_object *root = &opened->objects[0];
    SEXP index = R_NilValue, request, out;
    *lo = 0;
    *failed = 0;
    if (select != R_NilValue) {
        index = rdz_select_children(opened, select, e, failed, &request);
        if (*failed) return R_NilValue;
        if (request != R_NilValue) {
            *failed = 2;
            return request;
        }
    }
    PROTECT(index);
    if (window != R_NilValue) {
        double wlo = REAL(window)[0], whi = REAL(window)[1];
        uint16_t t = root->type_tag;
        zb_buf plan;
        if (t != RDZ_TYPE_LOGICAL && t != RDZ_TYPE_INTEGER && t != RDZ_TYPE_DOUBLE &&
            t != RDZ_TYPE_CHARACTER && t != RDZ_TYPE_FACTOR && t != RDZ_TYPE_DATA_FRAME) {
            UNPROTECT(1);
            *failed = 2;
            return rdz_req("rows_root", R_NilValue);
        }
        if (whi > (double)root->logical_len) {
            UNPROTECT(1);
            *failed = 2;
            return rdz_req("rows_range", Rf_ScalarReal((double)root->logical_len));
        }
        zb_buf_alloc(&plan, 0, 0); /* empty: cannot fail */
        if (rdz_window_fill(opened, wlo, whi, &plan, e)) {
            zb_buf_release(&plan);
            if (e->code != RDZ_E_UNSUPPORTED) {
                UNPROTECT(1);
                *failed = 1;
                return R_NilValue;
            }
            window = R_NilValue; /* refused: the value whole */
        } else {
            zb_buf_release(&plan);
            *lo = wlo;
        }
    }
    out = rdz_native_read_in(opened, threads, index, R_NilValue, window, e, failed);
    UNPROTECT(1);
    return out;
}

/* ---- one root attribute, read alone ---------------------------------------------- */

/* Attribute a's name into out (at most cap - 1 bytes): its kind's, or for a
   general attribute the one string of its name object, read from that
   object's one block. */
static int rdz_read_attr_name(rdz_reader *r, const rdz_attribute *a, char *out, size_t cap,
                              rdz_error *e)
{
    const rdz_object *o = &r->objects[a->name_object_id];
    const char *kind = a->flags == RDZ_ATTRIBUTE_FLAG_NAMES       ? "names"
                       : a->flags == RDZ_ATTRIBUTE_FLAG_ROW_NAMES ? "row.names"
                       : a->flags == RDZ_ATTRIBUTE_FLAG_CLASS     ? "class"
                                                                  : NULL;
    rdz_str s;
    size_t k;
    if (kind) {
        snprintf(out, cap, "%s", kind);
        return 0;
    }
    if (o->block_count != 1) return rdz_invalid(e, "an attribute name in more than one block");
    if (rdz_reader_read_block(r, o->first_block, &r->decoded, e) ||
        rdz_string_decode_records(r->decoded.data, r->decoded.len, 1, &s, e)) {
        return 1;
    }
    if (s.tag == RDZ_STR_NA || s.len == 0 || s.len >= cap) {
        return rdz_invalid(e, "an attribute's name is not a plain ASCII name");
    }
    for (k = 0; k < s.len; k++) {
        if (s.bytes[k] == 0 || (s.bytes[k] & 0x80u)) {
            return rdz_invalid(e, "an attribute's name is not a plain ASCII name");
        }
    }
    memcpy(out, s.bytes, s.len);
    out[s.len] = 0;
    return 0;
}

/* The root's attribute names as R would list them, implied ones included,
   reading only the general attributes' name blocks; NULL on failure. */
SEXP rdz_native_attribute_names(rdz_reader *r, rdz_error *e)
{
    const rdz_object *root;
    uint32_t k, implied = 0;
    SEXP out;
    char name[10001]; /* R caps a symbol at 10,000 bytes */
    if (!r->nobjects) return Rf_allocVector(STRSXP, 0);
    root = &r->objects[0];
    if (root->type_tag == RDZ_TYPE_FACTOR || root->type_tag == RDZ_TYPE_DATA_FRAME) {
        implied = root->type_tag == RDZ_TYPE_FACTOR ? 2 : 3;
    }
    out = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)implied + root->attribute_count));
    if (root->type_tag == RDZ_TYPE_FACTOR) {
        SET_STRING_ELT(out, 0, Rf_mkChar("levels"));
        SET_STRING_ELT(out, 1, Rf_mkChar("class"));
    } else if (root->type_tag == RDZ_TYPE_DATA_FRAME) {
        SET_STRING_ELT(out, 0, Rf_mkChar("names"));
        SET_STRING_ELT(out, 1, Rf_mkChar("row.names"));
        SET_STRING_ELT(out, 2, Rf_mkChar("class"));
    }
    for (k = 0; k < root->attribute_count; k++) {
        const rdz_attribute *a = &r->attributes[root->first_attribute + k];
        if (implied && a->flags != RDZ_ATTRIBUTE_FLAG_OTHER) continue; /* listed above */
        if (rdz_read_attr_name(r, a, name, sizeof name, e)) {
            UNPROTECT(1);
            return NULL;
        }
        SET_STRING_ELT(out, implied++, Rf_mkChar(name));
    }
    out = Rf_lengthgets(out, (R_xlen_t)implied);
    UNPROTECT(1);
    return out;
}

/* ---- the directory, and objects read alone ---------------------------------------- */

static void rdz_col(SEXP list, SEXP names, int i, const char *name, SEXP value)
{
    SET_VECTOR_ELT(list, i, value);
    SET_STRING_ELT(names, i, Rf_mkChar(name));
}

/* A native file's directory as two tables (lists of columns): objects (id,
   parent, role, type, flags, length, children, attributes, blocks, stored
   and decoded bytes) and attributes (owner, kind, name, name and value
   objects). General attributes' names come from their one-block name
   objects; nothing else is read. */
SEXP rdz_c_directory(SEXP path)
{
    rdz_reader *rp;
    rdz_error e;
    SEXP ptr = PROTECT(rdz_reader_handle(&rp));
    uint32_t i, n, na;
    SEXP out, outn, obj, objn, att, attn, col;
    int *id, *parent, *role, *type, *flags, *fc, *cc, *fa, *ac, *fb, *bc;
    double *len, *stored, *decoded;
    if (rdz_open_source(rp, path, &e)) {
        rdz_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    if (rp->codec_id != RDZ_CODEC_NATIVE_V1) {
        rdz_codec_error(&e, rp->codec_id, rp->codec_version);
        rdz_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    n = rp->nobjects;
    na = rp->nattributes;
    /* the reader belongs to `ptr`: an R error anywhere frees it */
    /* every list and column protected while it is filled (rchk cannot see
       through a list) */
    out = PROTECT(Rf_allocVector(VECSXP, 2));
    outn = PROTECT(Rf_allocVector(STRSXP, 2));
    obj = PROTECT(Rf_allocVector(VECSXP, 14));
    objn = PROTECT(Rf_allocVector(STRSXP, 14));
    rdz_col(out, outn, 0, "objects", obj);
    Rf_setAttrib(obj, R_NamesSymbol, objn);
#define RDZ_ICOL(k, nm, ptr)                                                  \
    col = PROTECT(Rf_allocVector(INTSXP, n));                                 \
    ptr = INTEGER(col);                                                       \
    rdz_col(obj, objn, k, nm, col);                                           \
    UNPROTECT(1)
#define RDZ_DCOL(k, nm, ptr)                                                  \
    col = PROTECT(Rf_allocVector(REALSXP, n));                                \
    ptr = REAL(col);                                                          \
    rdz_col(obj, objn, k, nm, col);                                           \
    UNPROTECT(1)
    RDZ_ICOL(0, "id", id);
    RDZ_ICOL(1, "parent", parent);
    RDZ_ICOL(2, "role", role);
    RDZ_ICOL(3, "type", type);
    RDZ_ICOL(4, "flags", flags);
    RDZ_DCOL(5, "length", len);
    RDZ_ICOL(6, "first_child", fc);
    RDZ_ICOL(7, "child_count", cc);
    RDZ_ICOL(8, "first_attribute", fa);
    RDZ_ICOL(9, "attribute_count", ac);
    RDZ_ICOL(10, "first_block", fb);
    RDZ_ICOL(11, "block_count", bc);
    RDZ_DCOL(12, "stored_bytes", stored);
    RDZ_DCOL(13, "decoded_bytes", decoded);
#undef RDZ_ICOL
#undef RDZ_DCOL
    for (i = 0; i < n; i++) {
        const rdz_object *o = &rp->objects[i];
        uint32_t b;
        id[i] = (int)i;
        parent[i] = o->parent_id == RDZ_ROOT_PARENT_ID ? NA_INTEGER : (int)o->parent_id;
        role[i] = o->role;
        type[i] = o->type_tag;
        flags[i] = (int)o->flags;
        len[i] = (double)o->logical_len;
        fc[i] = (int)o->first_child;
        cc[i] = (int)o->child_count;
        fa[i] = (int)o->first_attribute;
        ac[i] = (int)o->attribute_count;
        fb[i] = (int)o->first_block;
        bc[i] = (int)o->block_count;
        stored[i] = decoded[i] = 0;
        for (b = o->first_block; b < o->first_block + o->block_count; b++) {
            stored[i] += rp->blocks[b].stored_len;
            decoded[i] += (double)rp->blocks[b].decoded_len;
        }
    }
    att = PROTECT(Rf_allocVector(VECSXP, 5));
    attn = PROTECT(Rf_allocVector(STRSXP, 5));
    rdz_col(out, outn, 1, "attributes", att);
    Rf_setAttrib(att, R_NamesSymbol, attn);
    {
        SEXP owner = PROTECT(Rf_allocVector(INTSXP, na)), kind, name, nobj, vobj;
        rdz_col(att, attn, 0, "owner", owner);
        kind = PROTECT(Rf_allocVector(INTSXP, na));
        rdz_col(att, attn, 1, "kind", kind);
        name = PROTECT(Rf_allocVector(STRSXP, na));
        rdz_col(att, attn, 2, "name", name);
        nobj = PROTECT(Rf_allocVector(INTSXP, na));
        rdz_col(att, attn, 3, "name_object", nobj);
        vobj = PROTECT(Rf_allocVector(INTSXP, na));
        rdz_col(att, attn, 4, "value_object", vobj);
        for (i = 0; i < na; i++) {
            const rdz_attribute *a = &rp->attributes[i];
            char nm[10001]; /* R caps a symbol at 10,000 bytes */
            INTEGER(owner)[i] = (int)a->owner_id;
            INTEGER(kind)[i] = (int)a->flags;
            INTEGER(nobj)[i] = (int)a->name_object_id;
            INTEGER(vobj)[i] = (int)a->value_object_id;
            if (rdz_read_attr_name(rp, a, nm, sizeof nm, &e)) {
                rdz_reader_finalize(ptr);
                UNPROTECT(12);
                return rdz_failure(&e);
            }
            SET_STRING_ELT(name, i, Rf_mkChar(nm));
        }
        UNPROTECT(5);
    }
    rdz_reader_finalize(ptr);
    Rf_setAttrib(out, R_NamesSymbol, outn);
    UNPROTECT(7);
    return out;
}

/* Objects of a native file (0-based ids) and everything below each, read
   alone: the list of them. */
SEXP rdz_c_read_objects(SEXP path, SEXP ids, SEXP settings)
{
    rdz_reader r;
    rdz_error e;
    int failed, threads = INTEGER(settings)[1] < 1 ? 1 : INTEGER(settings)[1];
    SEXP out;
    if (rdz_open_source(&r, path, &e)) return rdz_failure(&e);
    if (r.codec_id != RDZ_CODEC_NATIVE_V1) {
        rdz_codec_error(&e, r.codec_id, r.codec_version);
        rdz_reader_close(&r);
        return rdz_failure(&e);
    }
    out = PROTECT(rdz_native_read_in(&r, threads, R_NilValue, ids, R_NilValue, &e, &failed)); /* closes r */
    UNPROTECT(1);
    return failed ? rdz_failure(&e) : out;
}
