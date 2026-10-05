/*
 * rdz_native_r.c -- the native codecs at the R boundary (plan-c Stages E
 * to G): which values they take, and the R objects they read back.
 *
 * Native: a logical, integer, double or character vector that is not
 * ALTREP, with no attribute or only `names`; or a factor (an integer vector
 * whose only attributes are `levels` and a class of "factor" or
 * c("ordered", "factor"), every code a level or NA). Strings are taken as
 * bytes plus R's encoding tag; a native-encoded non-ASCII string is not
 * portable and leaves the whole root to the generic codec (portability.md).
 * Messages for logical vectors are the Rust adapter's.
 *
 * Writing and reading run under R_UnwindProtect() with all state behind an
 * external pointer: an interrupt between blocks frees it at once. R thread
 * only.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>
#include <Rversion.h>

#include "../core/rdz_vector.h"
#include "../rdz_r.h"

/* R 4.6 removed ATTRIB() from the API; R_getAttribCount() replaces it. */
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

typedef struct {
    SEXP names;
} rdz_r_strings;

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
    return (uintptr_t)STRING_ELT(((rdz_r_strings *)ctx)->names, (R_xlen_t)i);
}

static int rdz_r_value(void *ctx, size_t i, rdz_str *out, rdz_error *e)
{
    SEXP c = STRING_ELT(((rdz_r_strings *)ctx)->names, (R_xlen_t)i);
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

/* Why x cannot be written natively, or NULL when it can. */
static const char *rdz_native_ineligible(SEXP x, SEXP *names, SEXP *levels, int *ordered)
{
    R_xlen_t count;
    int type = TYPEOF(x);
    *names = R_NilValue;
    *levels = R_NilValue;
    *ordered = 0;
    if (type != LGLSXP && type != INTSXP && type != REALSXP && type != STRSXP) {
        return Rf_type2char((SEXPTYPE)type);
    }
    if (ALTREP(x)) {
        return type == LGLSXP ? "an ALTREP logical vector"
               : type == INTSXP ? "an ALTREP integer vector"
               : type == REALSXP ? "an ALTREP double vector" : "an ALTREP character vector";
    }
    count = rdz_attribute_count(x);
    if (count == 0) return NULL;
    if (type == INTSXP && count == 2) {
        /* getAttrib() returns these stored attributes without allocating */
        SEXP cls = PROTECT(Rf_getAttrib(x, R_ClassSymbol));
        *levels = Rf_getAttrib(x, R_LevelsSymbol);
        UNPROTECT(1); /* both are x's own attributes, alive with x */
        if (TYPEOF(cls) == STRSXP && TYPEOF(*levels) == STRSXP && !ALTREP(*levels)) {
            R_xlen_t i, n = XLENGTH(x), nlev = XLENGTH(*levels);
            const int *codes = INTEGER_RO(x);
            if (XLENGTH(cls) == 1 && strcmp(CHAR(STRING_ELT(cls, 0)), "factor") == 0) {
                *ordered = 0;
            } else if (XLENGTH(cls) == 2 && strcmp(CHAR(STRING_ELT(cls, 0)), "ordered") == 0 &&
                       strcmp(CHAR(STRING_ELT(cls, 1)), "factor") == 0) {
                *ordered = 1;
            } else {
                *levels = R_NilValue;
                return "a classed integer vector";
            }
            for (i = 0; i < n; i++) {
                if (codes[i] != NA_INTEGER && (codes[i] < 1 || codes[i] > nlev)) {
                    *levels = R_NilValue;
                    return "a factor with codes outside its levels";
                }
            }
            return NULL;
        }
        *levels = R_NilValue;
    }
    /* for an atomic vector getAttrib() returns the stored names unallocated */
    *names = Rf_getAttrib(x, R_NamesSymbol);
    if (count != 1 || *names == R_NilValue) {
        return type == LGLSXP ? "a logical vector with attributes other than names"
                              : "a vector with attributes other than names";
    }
    if (TYPEOF(*names) != STRSXP || XLENGTH(*names) != XLENGTH(x)) {
        return type == LGLSXP ? "a malformed logical vector" : "a malformed vector";
    }
    if (ALTREP(*names)) return "a vector with ALTREP names";
    return NULL;
}

static void rdz_tick(void *ctx)
{
    (void)ctx;
    R_CheckUserInterrupt();
}

static void rdz_vec_finalize(SEXP ptr)
{
    rdz_vec *v = (rdz_vec *)R_ExternalPtrAddr(ptr);
    if (v) {
        rdz_vec_free(v);
        free(v);
        R_ClearExternalPtr(ptr);
    }
}

typedef struct {
    rdz_vec *v;
    const char *path;
    rdz_vec_spec spec;
    rdz_error e;
    int failed;
} rdz_write_call;

static SEXP rdz_write_body(void *data)
{
    rdz_write_call *c = (rdz_write_call *)data;
    c->failed = rdz_vec_write(c->v, c->path, &c->spec, &c->e);
    return R_NilValue;
}

static void rdz_vec_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_vec_finalize((SEXP)data);
}

static void rdz_source(rdz_str_source *src, rdz_r_strings *ctx, SEXP strings)
{
    ctx->names = strings;
    src->n = (size_t)XLENGTH(strings);
    src->ctx = ctx;
    src->key = rdz_r_key;
    src->value = rdz_r_value;
}

/* TRUE when written; FALSE when x is not for the native codecs and strict is
   FALSE (automatic mode then writes it generically); a failure otherwise.
   settings: c(level, threads, block size), as rdz_settings() gives them. */
SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy, SEXP settings)
{
    const char *p = rdz_path(path);
    SEXP names, levels, ptr, cont;
    int ordered;
    const char *why = rdz_native_ineligible(x, &names, &levels, &ordered);
    rdz_r_strings names_ctx, levels_ctx, values_ctx;
    rdz_str_source names_src, levels_src, values_src;
    rdz_write_call call;
    if (TYPEOF(settings) != INTSXP || XLENGTH(settings) != 3) {
        Rf_error("`settings` must be an integer vector of length 3.");
    }
    if (why) {
        if (!Rf_asLogical(strict)) return Rf_ScalarLogical(0);
        rdz_unsupported(&call.e, why);
        return rdz_failure(&call.e);
    }
    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, x));
    R_RegisterCFinalizerEx(ptr, rdz_vec_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    call.v = (rdz_vec *)malloc(sizeof(rdz_vec));
    if (!call.v) Rf_error("rdz could not allocate memory for a writer");
    rdz_vec_init(call.v);
    R_SetExternalPtrAddr(ptr, call.v);

    memset(&call.spec, 0, sizeof call.spec);
    call.path = p;
    call.spec.n = (size_t)XLENGTH(x);
    switch (TYPEOF(x)) {
    case LGLSXP:
        call.spec.type = RDZ_TYPE_LOGICAL;
        call.spec.values = LOGICAL_RO(x);
        break;
    case INTSXP:
        call.spec.type = levels != R_NilValue ? RDZ_TYPE_FACTOR : RDZ_TYPE_INTEGER;
        call.spec.values = INTEGER_RO(x);
        break;
    case REALSXP:
        call.spec.type = RDZ_TYPE_DOUBLE;
        call.spec.values = REAL_RO(x);
        break;
    default:
        call.spec.type = RDZ_TYPE_CHARACTER;
        rdz_source(&values_src, &values_ctx, x);
        call.spec.strings = &values_src;
        break;
    }
    if (levels != R_NilValue) {
        rdz_source(&levels_src, &levels_ctx, levels);
        call.spec.levels = &levels_src;
        call.spec.ordered = ordered;
    }
    if (names != R_NilValue) {
        rdz_source(&names_src, &names_ctx, names);
        call.spec.names = &names_src;
    }
    call.spec.policy = Rf_asInteger(policy);
    call.spec.level = INTEGER(settings)[0];
    call.spec.threads = INTEGER(settings)[1] < 1 ? 1 : INTEGER(settings)[1];
    call.spec.tick = rdz_tick;
    call.failed = 0;
    R_UnwindProtect(rdz_write_body, &call, rdz_vec_cleanup, ptr, cont);
    rdz_vec_finalize(ptr);
    UNPROTECT(2);
    if (!call.failed) return Rf_ScalarLogical(1);
    if (call.e.code == RDZ_E_UNSUPPORTED && !Rf_asLogical(strict)) return Rf_ScalarLogical(0);
    return rdz_failure(&call.e);
}

/* ---- reading ---------------------------------------------------------------------- */

typedef struct {
    SEXP target, dictionary;
    R_xlen_t filled, entries, length, dictionary_length;
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
    if ((R_xlen_t)count > s->length - s->filled) return rdz_invalid(e, "character object length mismatch");
    /* the core rejects an id at or past the entries delivered so far */
    for (i = 0; i < count; i++) {
        SET_STRING_ELT(s->target, s->filled + (R_xlen_t)i, STRING_ELT(s->dictionary, (R_xlen_t)ids[i]));
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
        if (r->blocks[i].encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) entries += r->blocks[i].logical_count;
    }
    return (size_t)entries;
}

/* Reads string object `object` into a new STRSXP; R_NilValue on failure. */
static SEXP rdz_strings_r(rdz_reader *r, rdz_vec *v, uint32_t object, int threads, rdz_error *e,
                          int *failed)
{
    rdz_r_names s;
    rdz_names_sink sink;
    size_t length = (size_t)r->objects[object].logical_len;
    *failed = 1;
    if (length > (size_t)R_XLEN_T_MAX) {
        rdz_limit(e, "allocation size");
        return R_NilValue;
    }
    s.target = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)length));
    s.dictionary = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)rdz_dictionary_length(r, object)));
    s.filled = 0;
    s.entries = 0;
    s.length = (R_xlen_t)length;
    s.dictionary_length = XLENGTH(s.dictionary);
    sink.ctx = &s;
    sink.plain = rdz_r_plain;
    sink.entries = rdz_r_entries;
    sink.indices = rdz_r_indices;
    if (rdz_vec_read_strings(v, r, object, &sink, threads, rdz_tick, NULL, e)) {
        UNPROTECT(2);
        return R_NilValue;
    }
    if (s.filled != s.length) {
        rdz_invalid(e, "character object length mismatch");
        UNPROTECT(2);
        return R_NilValue;
    }
    *failed = 0;
    UNPROTECT(2);
    return s.target;
}

/* The names of an open native file, or R_NilValue when it has none. The
   attribute's name object must say "names" (rdz_native_read_names checks). */
SEXP rdz_native_names_r(rdz_reader *r, rdz_vec *v, int threads, rdz_error *e, int *failed)
{
    int present;
    size_t length, dictionary_length;
    *failed = 0;
    if (rdz_native_names_info(r, &present, &length, &dictionary_length, e)) {
        *failed = 1;
        return R_NilValue;
    }
    if (!present) return R_NilValue;
    {
        /* the attribute's name, checked by reading it (one tiny block) */
        const rdz_object *name = &r->objects[r->attributes[0].name_object_id];
        rdz_str nm;
        if (name->block_count != 1 || rdz_reader_read_block(r, name->first_block, &r->decoded, e) ||
            rdz_string_decode_records(r->decoded.data, r->decoded.len, 1, &nm, e) ||
            nm.tag != RDZ_STR_NATIVE || nm.len != 5 || memcmp(nm.bytes, "names", 5) != 0) {
            if (e->code == RDZ_OK) rdz_invalid(e, "native attribute name is not names");
            *failed = 1;
            return R_NilValue;
        }
    }
    return rdz_strings_r(r, v, r->attributes[0].value_object_id, threads, e, failed);
}

/* A factor's levels and class, set on codes. */
static int rdz_factor_attributes(rdz_reader *r, rdz_vec *v, SEXP codes, int threads, rdz_error *e)
{
    int failed;
    SEXP levels, cls;
    R_xlen_t i, n = XLENGTH(codes), nlev;
    const int *c = INTEGER_RO(codes);
    levels = PROTECT(rdz_strings_r(r, v, 1, threads, e, &failed));
    if (failed) {
        UNPROTECT(1);
        return 1;
    }
    nlev = XLENGTH(levels);
    for (i = 0; i < n; i++) {
        if (c[i] != NA_INTEGER && (c[i] < 1 || c[i] > nlev)) {
            UNPROTECT(1);
            return rdz_invalid(e, "factor code outside its levels");
        }
    }
    if (r->objects[0].flags & RDZ_OBJECT_FLAG_ORDERED) {
        cls = PROTECT(Rf_allocVector(STRSXP, 2));
        SET_STRING_ELT(cls, 0, Rf_mkChar("ordered"));
        SET_STRING_ELT(cls, 1, Rf_mkChar("factor"));
    } else {
        cls = PROTECT(Rf_mkString("factor"));
    }
    Rf_setAttrib(codes, R_LevelsSymbol, levels);
    Rf_setAttrib(codes, R_ClassSymbol, cls);
    UNPROTECT(2);
    return 0;
}

/* The value of an open native file, read through v's pipeline with
   `threads` workers. Called under an unwind protection that frees v and the
   reader: rdz_tick() may jump. */
SEXP rdz_native_value_r(rdz_reader *r, rdz_vec *v, int threads, rdz_error *e, int *failed)
{
    uint16_t type;
    size_t n;
    SEXP x, names;
    void *out = NULL;
    *failed = 1;
    if (rdz_vec_shape(r, &type, &n, e)) return R_NilValue;
    if (n > (size_t)R_XLEN_T_MAX) {
        rdz_limit(e, "allocation size");
        return R_NilValue;
    }
    switch (type) {
    case RDZ_TYPE_LOGICAL:
        x = PROTECT(Rf_allocVector(LGLSXP, (R_xlen_t)n));
        out = LOGICAL(x);
        break;
    case RDZ_TYPE_INTEGER:
    case RDZ_TYPE_FACTOR:
        x = PROTECT(Rf_allocVector(INTSXP, (R_xlen_t)n));
        out = INTEGER(x);
        break;
    case RDZ_TYPE_DOUBLE:
        x = PROTECT(Rf_allocVector(REALSXP, (R_xlen_t)n));
        out = REAL(x);
        break;
    default:
        x = PROTECT(rdz_strings_r(r, v, 0, threads, e, failed));
        if (*failed) {
            UNPROTECT(1);
            return R_NilValue;
        }
        *failed = 1;
        break;
    }
    if (out && rdz_vec_read(v, r, out, threads, rdz_tick, NULL, e)) {
        UNPROTECT(1);
        return R_NilValue;
    }
    if (type == RDZ_TYPE_FACTOR && rdz_factor_attributes(r, v, x, threads, e)) {
        UNPROTECT(1);
        return R_NilValue;
    }
    names = PROTECT(rdz_native_names_r(r, v, threads, e, failed));
    if (*failed) {
        UNPROTECT(2);
        return R_NilValue;
    }
    if (names != R_NilValue) Rf_setAttrib(x, R_NamesSymbol, names);
    UNPROTECT(2);
    *failed = 0;
    return x;
}

typedef struct {
    rdz_reader r;
    rdz_vec v;
    const char *which;
    rdz_error e;
    int failed;
} rdz_attr_call;

static void rdz_attr_finalize(SEXP ptr)
{
    rdz_attr_call *c = (rdz_attr_call *)R_ExternalPtrAddr(ptr);
    if (c) {
        rdz_vec_free(&c->v);
        rdz_reader_close(&c->r);
        free(c);
        R_ClearExternalPtr(ptr);
    }
}

static SEXP rdz_attr_body(void *data)
{
    rdz_attr_call *c = (rdz_attr_call *)data;
    SEXP out;
    if (strcmp(c->which, "levels") == 0) {
        if (c->r.objects[0].type_tag != RDZ_TYPE_FACTOR) {
            c->failed = 1;
            rdz_invalid(&c->e, "the root has no levels");
            return R_NilValue;
        }
        return rdz_strings_r(&c->r, &c->v, 1, 1, &c->e, &c->failed);
    }
    if (strcmp(c->which, "class") == 0) {
        if (c->r.objects[0].type_tag != RDZ_TYPE_FACTOR) {
            c->failed = 1;
            rdz_invalid(&c->e, "the root has no class");
            return R_NilValue;
        }
        if (c->r.objects[0].flags & RDZ_OBJECT_FLAG_ORDERED) {
            out = PROTECT(Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(out, 0, Rf_mkChar("ordered"));
            SET_STRING_ELT(out, 1, Rf_mkChar("factor"));
            UNPROTECT(1);
            return out;
        }
        return Rf_mkString("factor");
    }
    out = rdz_native_names_r(&c->r, &c->v, 1, &c->e, &c->failed);
    return out == R_NilValue && !c->failed ? Rf_allocVector(STRSXP, 0) : out;
}

static void rdz_attr_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_attr_finalize((SEXP)data);
}

/* One attribute of a native file's root ("names", "levels" or "class"),
   reading only its own blocks. */
SEXP rdz_c_read_native_attribute(SEXP path, SEXP which)
{
    const char *p = rdz_path(path);
    SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    SEXP cont = PROTECT(R_MakeUnwindCont());
    SEXP out;
    rdz_attr_call *c;
    rdz_error e;
    R_RegisterCFinalizerEx(ptr, rdz_attr_finalize, TRUE);
    c = (rdz_attr_call *)calloc(1, sizeof *c);
    if (!c) Rf_error("rdz could not allocate memory for a reader");
    rdz_reader_init(&c->r);
    rdz_vec_init(&c->v);
    c->which = CHAR(STRING_ELT(which, 0));
    R_SetExternalPtrAddr(ptr, c);
    if (rdz_reader_open(&c->r, p, &c->e)) {
        e = c->e;
        rdz_attr_finalize(ptr);
        UNPROTECT(2);
        return rdz_failure(&e);
    }
    out = PROTECT(R_UnwindProtect(rdz_attr_body, c, rdz_attr_cleanup, ptr, cont));
    e = c->e;
    if (c->failed) {
        rdz_attr_finalize(ptr);
        UNPROTECT(3);
        return rdz_failure(&e);
    }
    rdz_attr_finalize(ptr);
    UNPROTECT(3);
    return out;
}
