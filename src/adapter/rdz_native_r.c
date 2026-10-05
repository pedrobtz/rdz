/*
 * rdz_native_r.c -- the native codec at the R boundary (plan-c Stage E):
 * which values it takes, and the R objects it reads back. Eligibility and
 * messages are the Rust adapter's: a logical vector that is not ALTREP,
 * with no attribute or only `names`, whose names are a non-ALTREP character
 * vector of the same length in a supported encoding.
 *
 * Writing calls no R function that allocates, so it cannot jump: the core
 * writer removes its temporary file on any failure. Reading allocates the
 * result on this thread, with the reader behind an external pointer.
 * R thread only.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>
#include <Rversion.h>

#include "../core/rdz_native.h"
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
        return rdz_unsupported(e, "a logical vector with a name larger than one block");
    }
    out->bytes = (const uint8_t *)CHAR(c);
    return 0;
}

/* Why x cannot be written natively, or NULL when it can; *names is set. */
static const char *rdz_native_ineligible(SEXP x, SEXP *names)
{
    R_xlen_t count;
    *names = R_NilValue;
    if (TYPEOF(x) != LGLSXP) return Rf_type2char(TYPEOF(x));
    if (ALTREP(x)) return "an ALTREP logical vector";
    count = rdz_attribute_count(x);
    if (count == 0) return NULL;
    /* for an atomic vector getAttrib() returns the stored names unallocated */
    *names = Rf_getAttrib(x, R_NamesSymbol);
    if (count != 1 || *names == R_NilValue) return "a logical vector with attributes other than names";
    if (TYPEOF(*names) != STRSXP || XLENGTH(*names) != XLENGTH(x)) {
        return "a malformed logical vector";
    }
    if (ALTREP(*names)) return "a logical vector with ALTREP names";
    return NULL;
}

/* TRUE when written; FALSE when x is not for the native codec and strict is
   FALSE (automatic mode then writes it generically); a failure otherwise. */
SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy)
{
    const char *p = rdz_path(path);
    SEXP names;
    const char *why = rdz_native_ineligible(x, &names);
    rdz_error e;
    rdz_r_strings ctx;
    rdz_str_source src;
    int failed;
    if (why) {
        if (!Rf_asLogical(strict)) return Rf_ScalarLogical(0);
        rdz_unsupported(&e, why);
        return rdz_failure(&e);
    }
    ctx.names = names;
    src.n = names == R_NilValue ? 0 : (size_t)XLENGTH(names);
    src.ctx = &ctx;
    src.key = rdz_r_key;
    src.value = rdz_r_value;
    failed = rdz_write_native_logical(p, LOGICAL_RO(x), (size_t)XLENGTH(x),
                                      names == R_NilValue ? NULL : &src, Rf_asInteger(policy), &e);
    if (!failed) return Rf_ScalarLogical(1);
    if (e.code == RDZ_E_UNSUPPORTED && !Rf_asLogical(strict)) return Rf_ScalarLogical(0);
    return rdz_failure(&e);
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

/* The names of an open native file, or R_NilValue when it has none; on
   failure, e is set and R_NilValue returned with *failed. */
SEXP rdz_native_names_r(rdz_reader *r, rdz_error *e, int *failed)
{
    int present;
    size_t length, dictionary_length;
    rdz_r_names s;
    rdz_names_sink sink;
    *failed = 0;
    if (rdz_native_names_info(r, &present, &length, &dictionary_length, e)) {
        *failed = 1;
        return R_NilValue;
    }
    if (!present) return R_NilValue;
    s.target = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)length));
    s.dictionary = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)dictionary_length));
    s.filled = 0;
    s.entries = 0;
    s.length = (R_xlen_t)length;
    s.dictionary_length = (R_xlen_t)dictionary_length;
    sink.ctx = &s;
    sink.plain = rdz_r_plain;
    sink.entries = rdz_r_entries;
    sink.indices = rdz_r_indices;
    if (rdz_native_read_names(r, &sink, e) || s.filled != s.length) {
        if (s.filled != s.length && e->code == RDZ_OK) rdz_invalid(e, "character object length mismatch");
        *failed = 1;
        UNPROTECT(2);
        return R_NilValue;
    }
    UNPROTECT(2);
    return s.target;
}

/* The value of an open native file. */
SEXP rdz_native_value_r(rdz_reader *r, rdz_error *e, int *failed)
{
    size_t n;
    SEXP x, names;
    *failed = 1;
    if (rdz_native_length(r, &n, e)) return R_NilValue;
    if (n > (size_t)R_XLEN_T_MAX) {
        rdz_limit(e, "allocation size");
        return R_NilValue;
    }
    x = PROTECT(Rf_allocVector(LGLSXP, (R_xlen_t)n));
    if (rdz_native_read_logical(r, LOGICAL(x), e)) {
        UNPROTECT(1);
        return R_NilValue;
    }
    names = PROTECT(rdz_native_names_r(r, e, failed));
    if (*failed) {
        UNPROTECT(2);
        return R_NilValue;
    }
    if (names != R_NilValue) Rf_setAttrib(x, R_NamesSymbol, names);
    UNPROTECT(2);
    *failed = 0;
    return x;
}

static void rdz_native_reader_finalize(SEXP ptr)
{
    rdz_reader *r = (rdz_reader *)R_ExternalPtrAddr(ptr);
    if (r) {
        rdz_reader_close(r);
        free(r);
        R_ClearExternalPtr(ptr);
    }
}

/* The names of a native file, read without its logical blocks. */
SEXP rdz_c_read_native_names(SEXP path)
{
    const char *p = rdz_path(path);
    SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    SEXP out;
    rdz_reader *r;
    rdz_error e;
    int failed;
    R_RegisterCFinalizerEx(ptr, rdz_native_reader_finalize, TRUE);
    r = (rdz_reader *)malloc(sizeof *r);
    if (!r) Rf_error("rdz could not allocate memory for a reader");
    rdz_reader_init(r);
    R_SetExternalPtrAddr(ptr, r);
    if (rdz_reader_open(r, p, &e)) {
        rdz_native_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    out = PROTECT(rdz_native_names_r(r, &e, &failed));
    rdz_native_reader_finalize(ptr);
    UNPROTECT(2);
    if (failed) return rdz_failure(&e);
    return out == R_NilValue ? Rf_allocVector(STRSXP, 0) : out;
}
