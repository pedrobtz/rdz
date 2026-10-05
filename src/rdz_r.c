/*
 * rdz_r.c -- the .Call entry points over the C core.
 *
 * C never raises an rdz error: a failure comes back as an "rdz_failure"
 * string holding the message, with the kind in attribute "kind", and
 * R/c-core.R turns it into a classed condition. R's own errors (allocation,
 * an untranslatable path) arrive as R raises them; every core allocation is
 * owned by an external pointer created before the first one, so its
 * finalizer releases it however R unwinds (plan-c.md section 4).
 */
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>

#include "core/rdz_logical.h"
#include "rdz_r.h"

static const char *rdz_kind(rdz_code code)
{
    switch (code) {
    case RDZ_E_LIMIT: return "limit";
    case RDZ_E_VERSION: return "version";
    case RDZ_E_CODEC: return "codec";
    case RDZ_E_IO: return "io";
    case RDZ_E_MEMORY: return "memory";
    case RDZ_E_UNSUPPORTED: return "unsupported";
    default: return "invalid";
    }
}

SEXP rdz_failure(const rdz_error *e)
{
    SEXP out = PROTECT(Rf_mkString(e->message));
    SEXP kind = PROTECT(Rf_mkString(rdz_kind(e->code)));
    SEXP cls = PROTECT(Rf_mkString("rdz_failure"));
    Rf_setAttrib(out, Rf_install("kind"), kind);
    Rf_setAttrib(out, R_ClassSymbol, cls);
    UNPROTECT(3);
    return out;
}

/* The bytes of a path for the core: UTF-8 on Windows, where the core opens
   it through the wide-character API; the native encoding elsewhere. */
const char *rdz_path(SEXP path)
{
    if (TYPEOF(path) != STRSXP || XLENGTH(path) != 1 || STRING_ELT(path, 0) == NA_STRING) {
        Rf_error("`path` must be a single, non-missing string.");
    }
#ifdef _WIN32
    return Rf_translateCharUTF8(STRING_ELT(path, 0));
#else
    return Rf_translateChar(STRING_ELT(path, 0));
#endif
}

static void rdz_reader_finalize(SEXP ptr)
{
    rdz_reader *r = (rdz_reader *)R_ExternalPtrAddr(ptr);
    if (r) {
        rdz_reader_close(r);
        free(r);
        R_ClearExternalPtr(ptr);
    }
}

/* An external pointer owning a closed reader, protected by the caller. */
static SEXP rdz_reader_handle(rdz_reader **out)
{
    SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    rdz_reader *r;
    R_RegisterCFinalizerEx(ptr, rdz_reader_finalize, TRUE);
    r = (rdz_reader *)malloc(sizeof *r);
    if (!r) Rf_error("rdz could not allocate memory for a reader");
    rdz_reader_init(r);
    R_SetExternalPtrAddr(ptr, r);
    *out = r;
    UNPROTECT(1);
    return ptr;
}

void rdz_raise(const rdz_error *e)
{
    SEXP failure = PROTECT(rdz_failure(e));
    SEXP name = PROTECT(Rf_mkString("rdz"));
    SEXP ns = PROTECT(R_FindNamespace(name));
    SEXP call = PROTECT(Rf_lang2(Rf_install("rdz_check"), failure));
    Rf_eval(call, ns);
    UNPROTECT(4); /* not reached: rdz_check() raises */
}

static void rdz_set(SEXP list, SEXP names, R_xlen_t i, const char *name, SEXP value)
{
    SET_VECTOR_ELT(list, i, value);
    SET_STRING_ELT(names, i, Rf_mkChar(name));
}

SEXP rdz_c_info(SEXP path)
{
    const char *p = rdz_path(path);
    rdz_reader *r;
    rdz_error e;
    SEXP ptr = PROTECT(rdz_reader_handle(&r));
    SEXP out, names, synopsis;
    const char *codec = "unknown";

    if (rdz_reader_open(r, p, &e)) {
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    rdz_infile_close(&r->file);
    if (r->codec_id == RDZ_CODEC_R_SERIAL_V3) codec = "r_serial_v3";
    if (r->codec_id == RDZ_CODEC_NATIVE_V1) codec = "native_v1";

    out = PROTECT(Rf_allocVector(VECSXP, 14));
    names = PROTECT(Rf_allocVector(STRSXP, 14));
    rdz_set(out, names, 0, "container_version", Rf_ScalarInteger(r->container_version));
    rdz_set(out, names, 1, "codec", Rf_mkString(codec));
    rdz_set(out, names, 2, "codec_id", Rf_ScalarInteger(r->codec_id));
    rdz_set(out, names, 3, "codec_version", Rf_ScalarInteger(r->codec_version));
    rdz_set(out, names, 4, "block_size", Rf_ScalarInteger((int)r->block_size));
    rdz_set(out, names, 5, "block_count", Rf_ScalarInteger((int)r->nblocks));
    rdz_set(out, names, 6, "object_count", Rf_ScalarInteger((int)r->nobjects));
    rdz_set(out, names, 7, "attribute_count", Rf_ScalarInteger((int)r->nattributes));
    rdz_set(out, names, 8, "payload_bytes", Rf_ScalarReal((double)r->payload_bytes));
    rdz_set(out, names, 9, "file_bytes", Rf_ScalarReal((double)r->file_bytes));
    synopsis = PROTECT(Rf_allocVector(RAWSXP, r->synopsis_len));
    if (r->synopsis_len) memcpy(RAW(synopsis), r->synopsis, r->synopsis_len);
    rdz_set(out, names, 10, "synopsis", synopsis);
    UNPROTECT(1);
    rdz_set(out, names, 11, "root_type",
            Rf_mkString(!r->nobjects ? ""
                        : r->objects[0].type_tag == RDZ_TYPE_INTEGER ? "integer"
                        : r->objects[0].type_tag == RDZ_TYPE_DOUBLE ? "double"
                        : r->objects[0].type_tag == RDZ_TYPE_CHARACTER ? "character"
                        : r->objects[0].type_tag == RDZ_TYPE_FACTOR ? "factor" : "logical"));
    rdz_set(out, names, 12, "root_length",
            Rf_ScalarReal(r->nobjects ? (double)r->objects[0].logical_len : -1.0));
    if (r->nobjects && r->objects[0].type_tag == RDZ_TYPE_FACTOR) {
        SEXP an = Rf_allocVector(STRSXP, 2);
        rdz_set(out, names, 13, "attribute_names", an);
        SET_STRING_ELT(an, 0, Rf_mkChar("levels"));
        SET_STRING_ELT(an, 1, Rf_mkChar("class"));
    } else {
        rdz_set(out, names, 13, "attribute_names",
                r->nattributes ? Rf_mkString("names") : Rf_allocVector(STRSXP, 0));
    }
    Rf_setAttrib(out, R_NamesSymbol, names);

    rdz_reader_finalize(ptr);
    UNPROTECT(3);
    return out;
}

SEXP rdz_c_has_rust(void)
{
#ifdef RDZ_HAVE_RUST
    return Rf_ScalarLogical(1);
#else
    return Rf_ScalarLogical(0);
#endif
}

SEXP rdz_generic_write(SEXP x, SEXP synopsis, SEXP path, SEXP settings, int fail_after);
SEXP rdz_generic_read(SEXP path, SEXP settings);

SEXP rdz_c_write_generic(SEXP x, SEXP synopsis, SEXP path, SEXP settings)
{
    return rdz_generic_write(x, synopsis, path, settings, -1);
}

SEXP rdz_c_read(SEXP path, SEXP settings)
{
    return rdz_generic_read(path, settings);
}

/* The logical classifier in use; force_scalar TRUE or FALSE switches the
   scalar reference on or off first, NA leaves it (tests compare kernels). */
SEXP rdz_c_logical_kernel(SEXP force_scalar)
{
    int f = Rf_asLogical(force_scalar);
    if (f != NA_LOGICAL) rdz_logical_force_scalar(f);
    return Rf_mkString(rdz_logical_kernel());
}

SEXP rdz_c_zstd_version(void)
{
    return Rf_mkString(rdz_codec_zstd_version());
}

/* ---- test-only entry points (rdz_test_) ---------------------------------------- */

/* The streamed writer, raising an R error after `blocks` blocks. */
SEXP rdz_test_write_generic_unwind(SEXP x, SEXP path, SEXP blocks, SEXP settings)
{
    SEXP synopsis = PROTECT(Rf_allocVector(RAWSXP, 0));
    SEXP out = rdz_generic_write(x, synopsis, path, settings, Rf_asInteger(blocks));
    UNPROTECT(1);
    return out;
}

SEXP rdz_test_records(void)
{
    const char *record = NULL;
    return rdz_records_check(&record) ? Rf_mkString(record) : R_NilValue;
}

/* The concatenated decoded bytes of a generic file, every block verified. */
SEXP rdz_test_read_generic(SEXP path)
{
    const char *p = rdz_path(path);
    rdz_reader *r;
    rdz_error e;
    SEXP ptr = PROTECT(rdz_reader_handle(&r));
    SEXP out;
    zb_buf window;
    uint32_t i;
    size_t at = 0;

    if (rdz_reader_open(r, p, &e)) {
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    if (r->codec_id != RDZ_CODEC_R_SERIAL_V3) {
        rdz_codec_error(&e, r->codec_id, r->codec_version);
        rdz_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    if (r->payload_bytes > (uint64_t)R_XLEN_T_MAX) {
        rdz_limit(&e, "payload length");
        rdz_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    out = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t)r->payload_bytes));
    for (i = 0; i < r->nblocks; i++) {
        /* each block is read straight into its place in the R vector */
        zb_buf_borrow(&window, RAW(out) + at, (size_t)r->blocks[i].decoded_len);
        if (rdz_reader_read_block(r, i, &window, &e)) {
            rdz_reader_finalize(ptr);
            UNPROTECT(2);
            return rdz_failure(&e);
        }
        at += (size_t)r->blocks[i].decoded_len;
    }
    rdz_reader_finalize(ptr);
    UNPROTECT(2);
    return out;
}

SEXP rdz_test_write_generic(SEXP payload, SEXP synopsis, SEXP path)
{
    const char *p = rdz_path(path);
    rdz_error e;
    if (TYPEOF(payload) != RAWSXP || TYPEOF(synopsis) != RAWSXP) {
        Rf_error("`payload` and `synopsis` must be raw vectors.");
    }
    /* No R allocation happens between the temporary file's creation and its
       commit or removal, so the writer needs no external pointer here. */
    if (rdz_write_generic(p, RAW(payload), (size_t)XLENGTH(payload), RAW(synopsis),
                          (size_t)XLENGTH(synopsis), &e)) {
        return rdz_failure(&e);
    }
    return R_NilValue;
}
