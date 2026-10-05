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
 * attribute, omitted on write (and restored by R when read). Anything else
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

#include "../core/rdz_graph.h"
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

/* ---- strings ---------------------------------------------------------------------- */

/* Rf_charIsASCII() entered R's API after the 4.1 floor: scan instead. */
static int rdz_ascii(SEXP c)
{
    const unsigned char *p = (const unsigned char *)CHAR(c);
    int i, n = LENGTH(c);
    for (i = 0; i < n; i++) {
        if (p[i] & 0x80u) return 0;
    }
    return 1;
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
            return rdz_unsupported(e, "a logical vector with non-ASCII native-encoded names");
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
static rdz_node *rdz_plan_add(rdz_plan *p, SEXP x, const char *name, uint16_t role,
                              uint32_t parent, const char **why)
{
    rdz_node *n;
    SEXP *slot;
    uint32_t d;
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
        R_xlen_t k, nlev;
        const int *codes;
        int ordered = rdz_is_class(a.cls, "ordered", "factor");
        if (!ordered && !rdz_is_class(a.cls, "factor", NULL)) {
            return "a factor whose class is not \"factor\" or c(\"ordered\", \"factor\")";
        }
        if (a.names != R_NilValue || a.row_names != R_NilValue || TYPEOF(levels) != STRSXP ||
            rdz_attribute_count(levels) != 0) {
            return "a factor with names or malformed levels";
        }
        nlev = XLENGTH(levels);
        codes = INTEGER_RO(x);
        for (k = 0; k < XLENGTH(x); k++) {
            if (codes[k] != NA_INTEGER && (codes[k] < 1 || codes[k] > nlev)) {
                return "a factor with codes outside its levels";
            }
        }
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
        /* compact row names are c(NA_integer_, n) or c(NA_integer_, -n) */
        compact = TYPEOF(a.row_names) == INTSXP && XLENGTH(a.row_names) == 2 &&
                  INTEGER_RO(a.row_names)[0] == NA_INTEGER;
        if (compact) {
            int m = INTEGER_RO(a.row_names)[1];
            nrow = (uint64_t)(m < 0 ? -(int64_t)m : m);
        } else if (TYPEOF(a.row_names) == INTSXP || TYPEOF(a.row_names) == STRSXP) {
            nrow = (uint64_t)XLENGTH(a.row_names);
        } else {
            return "a data frame with malformed row names";
        }
        for (k = 0; k < ncol; k++) {
            if ((uint64_t)Rf_xlength(VECTOR_ELT(x, k)) != nrow) {
                return "a data frame whose columns differ in length";
            }
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
SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy, SEXP settings)
{
    const char *p = rdz_path(path);
    SEXP ptr, cont;
    rdz_write_call call;
    const char *why;
    if (TYPEOF(settings) != INTSXP || XLENGTH(settings) != 3) {
        Rf_error("`settings` must be an integer vector of length 3.");
    }
    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, x));
    R_RegisterCFinalizerEx(ptr, rdz_plan_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    call.p = (rdz_plan *)calloc(1, sizeof(rdz_plan));
    if (!call.p) Rf_error("rdz could not allocate memory for a writer");
    rdz_vec_init(&call.p->v);
    zb_buf_alloc(&call.p->nodes, 0, 0);
    zb_buf_alloc(&call.p->objects, 0, 0);
    zb_buf_alloc(&call.p->names, 0, 0);
    zb_buf_alloc(&call.p->depth, 0, 0);
    zb_buf_alloc(&call.p->attrs, 0, 0);
    zb_buf_alloc(&call.p->sources, 0, 0);
    zb_buf_alloc(&call.p->pairs, 0, 0);
    R_SetExternalPtrAddr(ptr, call.p);

    why = rdz_plan_build(call.p, x);
    if (!why && !Rf_asLogical(strict) && rdz_plan_small_parts(call.p)) {
        why = "an object of many small parts";
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
    int threads;
    rdz_error e;
    int failed;
} rdz_graph_in;

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

/* The attribute name object's one string as a symbol name: ASCII, native or
   UTF-8, not empty (what writers write). NULL when it is not. */
static const char *rdz_attr_name(SEXP nm)
{
    SEXP c = STRING_ELT(nm, 0);
    if (c == NA_STRING || LENGTH(c) == 0 || !rdz_ascii(c)) return NULL;
    return CHAR(c);
}

/* Builds the whole value of an open native file. */
static SEXP rdz_graph_body(void *data)
{
    rdz_graph_in *g = (rdz_graph_in *)data;
    rdz_reader *r = &g->r;
    uint32_t i, n = r->nobjects, k;
    rdz_graph_sinks sinks;
    rdz_r_names *names;
    g->failed = 1;
    for (i = 0; i < n; i++) {
        const rdz_object *o = &r->objects[i];
        SEXP x;
        if (o->logical_len > (uint64_t)R_XLEN_T_MAX || o->child_count > (uint32_t)INT_MAX) {
            rdz_limit(&g->e, "allocation size");
            return R_NilValue;
        }
        switch (o->type_tag) {
        case RDZ_TYPE_LOGICAL: x = Rf_allocVector(LGLSXP, (R_xlen_t)o->logical_len); break;
        case RDZ_TYPE_INTEGER:
        case RDZ_TYPE_FACTOR: x = Rf_allocVector(INTSXP, (R_xlen_t)o->logical_len); break;
        case RDZ_TYPE_DOUBLE: x = Rf_allocVector(REALSXP, (R_xlen_t)o->logical_len); break;
        case RDZ_TYPE_CHARACTER: x = Rf_allocVector(STRSXP, (R_xlen_t)o->logical_len); break;
        case RDZ_TYPE_LIST:
        case RDZ_TYPE_DATA_FRAME: x = Rf_allocVector(VECSXP, (R_xlen_t)o->child_count); break;
        default: x = R_NilValue; break;
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
    if (rdz_graph_read(&g->v, r, &sinks, g->threads, rdz_tick, NULL, &g->e)) return R_NilValue;

    /* last to first: every child, level and attribute value comes after
       its owner, so each is complete before it is attached */
    names = (rdz_r_names *)(void *)g->sinks.data;
    for (i = n; i-- > 0;) {
        const rdz_object *o = &r->objects[i];
        SEXP x = VECTOR_ELT(g->holder, i);
        int have_rn = 0, have_class = 0;
        if (o->type_tag == RDZ_TYPE_CHARACTER && names[i].filled != names[i].length) {
            rdz_invalid(&g->e, "character object length mismatch");
            return R_NilValue;
        }
        if (o->type_tag == RDZ_TYPE_LIST || o->type_tag == RDZ_TYPE_DATA_FRAME) {
            for (k = 0; k < o->child_count; k++) {
                SET_VECTOR_ELT(x, (R_xlen_t)k, VECTOR_ELT(g->holder, o->first_child + k));
            }
        }
        if (o->type_tag == RDZ_TYPE_FACTOR) {
            SEXP levels = VECTOR_ELT(g->holder, o->first_child), cls;
            R_xlen_t j, nlev = XLENGTH(levels);
            const int *c = INTEGER_RO(x);
            for (j = 0; j < XLENGTH(x); j++) {
                if (c[j] != NA_INTEGER && (c[j] < 1 || c[j] > nlev)) {
                    rdz_invalid(&g->e, "factor code outside its levels");
                    return R_NilValue;
                }
            }
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
            SEXP nm = VECTOR_ELT(g->holder, a->name_object_id);
            SEXP value = VECTOR_ELT(g->holder, a->value_object_id);
            const char *name = rdz_attr_name(nm);
            rdz_set_attr set;
            int refused = 0;
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
                INTEGER(rn)[1] = -(int)o->logical_len;
                Rf_setAttrib(x, R_RowNamesSymbol, rn);
                UNPROTECT(1);
            }
            if (!have_class) {
                SEXP cls = PROTECT(Rf_mkString("data.frame"));
                Rf_setAttrib(x, R_ClassSymbol, cls);
                UNPROTECT(1);
            }
        }
    }
    g->failed = 0;
    return VECTOR_ELT(g->holder, 0);
}

static void rdz_graph_in_finalize(SEXP ptr)
{
    rdz_graph_in *g = (rdz_graph_in *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_vec_free(&g->v);
        rdz_reader_close(&g->r);
        zb_buf_release(&g->sinks);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

static void rdz_graph_in_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_graph_in_finalize((SEXP)data);
}

/* The value of a native file, given its open reader, which this takes over
   (its buffers and file) and closes. */
SEXP rdz_native_read_r(rdz_reader *opened, int threads, rdz_error *e, int *failed)
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
    g->threads = threads;
    R_SetExternalPtrAddr(ptr, g);
    holder = Rf_allocVector(VECSXP, 2 * (R_xlen_t)g->r.nobjects);
    R_SetExternalPtrProtected(ptr, holder);
    g->holder = holder;
    if (zb_buf_alloc(&g->sinks, 0, 0) ||
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

/* The root attribute `which` of a native file (names, levels, class or
   row.names), reading only the blocks of the object that holds it. */
SEXP rdz_c_read_native_attribute(SEXP path, SEXP which)
{
    const char *p = rdz_path(path), *w = CHAR(STRING_ELT(which, 0));
    rdz_reader r, view;
    rdz_error e;
    const rdz_object *root;
    rdz_object only;
    uint32_t k, object = 0;
    int failed;
    SEXP out;

    if (rdz_reader_open(&r, p, &e)) return rdz_failure(&e);
    if (r.codec_id != RDZ_CODEC_NATIVE_V1) {
        rdz_codec_error(&e, r.codec_id, r.codec_version);
        rdz_reader_close(&r);
        return rdz_failure(&e);
    }
    root = &r.objects[0];
    if (strcmp(w, "levels") == 0 && root->type_tag == RDZ_TYPE_FACTOR) object = root->first_child;
    for (k = 0; !object && k < root->attribute_count; k++) {
        const rdz_attribute *a = &r.attributes[root->first_attribute + k];
        char name[10001]; /* R caps a symbol at 10,000 bytes */
        if (rdz_read_attr_name(&r, a, name, sizeof name, &e)) {
            rdz_reader_close(&r);
            return rdz_failure(&e);
        }
        if (strcmp(w, name) == 0) object = a->value_object_id;
    }
    if (object && (r.objects[object].child_count || r.objects[object].attribute_count)) {
        /* a value with parts of its own: read with the whole object */
        SEXP x, sym = Rf_install(w);
        x = PROTECT(rdz_native_read_r(&r, 1, &e, &failed)); /* closes r */
        if (failed) {
            UNPROTECT(1);
            return rdz_failure(&e);
        }
        out = Rf_getAttrib(x, sym);
        UNPROTECT(1);
        return out;
    }
    if (!object) {
        /* attributes implied by the root rather than stored */
        uint16_t t = root->type_tag;
        uint32_t flags = root->flags;
        uint64_t nrow = root->logical_len, j;
        rdz_reader_close(&r);
        if (strcmp(w, "class") == 0 && t == RDZ_TYPE_FACTOR) {
            if (flags & RDZ_OBJECT_FLAG_ORDERED) {
                out = PROTECT(Rf_allocVector(STRSXP, 2));
                SET_STRING_ELT(out, 0, Rf_mkChar("ordered"));
                SET_STRING_ELT(out, 1, Rf_mkChar("factor"));
                UNPROTECT(1);
                return out;
            }
            return Rf_mkString("factor");
        }
        if (strcmp(w, "class") == 0 && t == RDZ_TYPE_DATA_FRAME) return Rf_mkString("data.frame");
        if (strcmp(w, "row.names") == 0 && t == RDZ_TYPE_DATA_FRAME && nrow <= (uint64_t)INT_MAX) {
            out = PROTECT(Rf_allocVector(INTSXP, (R_xlen_t)nrow));
            for (j = 0; j < nrow; j++) INTEGER(out)[j] = (int)(j + 1);
            UNPROTECT(1);
            return out;
        }
        return strcmp(w, "names") == 0 ? Rf_allocVector(STRSXP, 0) : R_NilValue;
    }
    /* a one-object view of the file: that object as the root, its blocks
       renumbered from zero (block entries keep their file offsets) */
    view = r;
    only = r.objects[object];
    view.blocks = r.blocks + only.first_block;
    view.nblocks = only.block_count;
    only.object_id = 0;
    only.parent_id = RDZ_ROOT_PARENT_ID;
    only.role = RDZ_ROLE_ROOT;
    only.first_attribute = 0;
    only.attribute_count = 0;
    only.first_block = 0;
    view.objects = &only;
    view.nobjects = 1;
    view.nattributes = 0;
    out = PROTECT(rdz_native_read_r(&view, 1, &e, &failed)); /* closes the view's buffers */
    UNPROTECT(1);
    if (failed) return rdz_failure(&e);
    return out;
}
