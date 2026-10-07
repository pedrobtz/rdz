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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>

#include <zubin/rw.h>

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

SEXP rdz_request(const char *what, SEXP data)
{
    SEXP out = PROTECT(Rf_mkString(what));
    Rf_setAttrib(out, Rf_install("kind"), PROTECT(Rf_mkString("request")));
    Rf_setAttrib(out, Rf_install("data"), data);
    Rf_setAttrib(out, R_ClassSymbol, PROTECT(Rf_mkString("rdz_failure")));
    UNPROTECT(3);
    return out;
}

const char *rdz_root_type_name(const rdz_reader *r)
{
    if (!r->nobjects) return "";
    switch (r->objects[0].type_tag) {
    case RDZ_TYPE_INTEGER: return "integer";
    case RDZ_TYPE_DOUBLE: return "double";
    case RDZ_TYPE_CHARACTER: return "character";
    case RDZ_TYPE_FACTOR: return "factor";
    case RDZ_TYPE_LIST: return "list";
    case RDZ_TYPE_DATA_FRAME: return "data.frame";
    default: return "logical"; /* as rdz_info() has always said, NULL included */
    }
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

/* Opens what R names: a path, or a raw vector holding a file (read in
   place: the caller keeps it alive for the call). */
/* Files opened through rdz_open_source(), for rdz_test_opens(): R thread
   only, like everything in this file. */
static double rdz_opens = 0;

int rdz_open_source(rdz_reader *r, SEXP src, rdz_error *e)
{
    rdz_opens++;
    if (TYPEOF(src) == RAWSXP) return rdz_reader_open_memory(r, RAW(src), (size_t)XLENGTH(src), e);
    return rdz_reader_open(r, rdz_path(src), e);
}

void rdz_reader_finalize(SEXP ptr)
{
    rdz_reader *r = (rdz_reader *)R_ExternalPtrAddr(ptr);
    if (r) {
        rdz_reader_close(r);
        free(r);
        R_ClearExternalPtr(ptr);
    }
}

/* An external pointer owning a closed reader, protected by the caller. */
SEXP rdz_reader_handle(rdz_reader **out)
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

SEXP rdz_native_attribute_names(rdz_reader *r, rdz_error *e); /* adapter/rdz_native_r.c */

/* rdz_info()'s fields of an open reader, which stays open; NULL (and e)
   when a native file's attribute names cannot be read. */
SEXP rdz_info_list(rdz_reader *r, rdz_error *e)
{
    SEXP out, names, synopsis;
    const char *codec = "unknown";

    if (r->codec_id == RDZ_CODEC_R_SERIAL_V3) codec = "r_serial_v3";
    if (r->codec_id == RDZ_CODEC_NATIVE_V1) codec = "native_v1";

    out = PROTECT(Rf_allocVector(VECSXP, 17));
    names = PROTECT(Rf_allocVector(STRSXP, 17));
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
    rdz_set(out, names, 11, "root_type", Rf_mkString(rdz_root_type_name(r)));
    rdz_set(out, names, 12, "root_length",
            Rf_ScalarReal(!r->nobjects ? -1.0
                          : r->objects[0].type_tag == RDZ_TYPE_LIST ||
                                    r->objects[0].type_tag == RDZ_TYPE_DATA_FRAME
                              ? (double)r->objects[0].child_count
                              : (double)r->objects[0].logical_len));
    if (r->codec_id == RDZ_CODEC_NATIVE_V1) {
        SEXP an = rdz_native_attribute_names(r, e);
        if (!an) {
            UNPROTECT(2);
            return NULL;
        }
        PROTECT(an);
        rdz_set(out, names, 13, "attribute_names", an);
        UNPROTECT(1);
    } else {
        rdz_set(out, names, 13, "attribute_names", Rf_allocVector(STRSXP, 0));
    }
    {
        /* "rdz 0.1.0", "rdz 0.1.0 (development)", "" when not recorded */
        char w[64], who[16];
        unsigned impl = r->writer[0] & ~RDZ_WRITER_DEV & 0xffu;
        if (impl == RDZ_WRITER_RDZ) {
            snprintf(who, sizeof who, "rdz");
        } else {
            snprintf(who, sizeof who, "writer %u", impl);
        }
        w[0] = 0;
        if (r->writer[0] != 0) {
            snprintf(w, sizeof w, "%s %u.%u.%u%s", who, r->writer[1], r->writer[2], r->writer[3],
                     (r->writer[0] & RDZ_WRITER_DEV) ? " (development)" : "");
        }
        rdz_set(out, names, 14, "writer", Rf_mkString(w));
    }
    if (r->hash_scheme == RDZ_CONTENT_HASH_V1) {
        rdz_set(out, names, 15, "content_hash", rdz_hash_text(r->content_hash));
    } else {
        rdz_set(out, names, 15, "content_hash", Rf_ScalarString(NA_STRING));
    }
    {
        /* the metadata as a named UTF-8 character vector (checked on opening) */
        SEXP md = PROTECT(Rf_allocVector(STRSXP, r->metadata_count));
        SEXP mn = PROTECT(Rf_allocVector(STRSXP, r->metadata_count));
        size_t at = 4;
        uint32_t k;
        for (k = 0; k < r->metadata_count; k++) {
            uint32_t n = zb_rd_u32le(r->metadata + at);
            SET_STRING_ELT(mn, k, Rf_mkCharLenCE((const char *)r->metadata + at + 4, (int)n, CE_UTF8));
            at += 4 + n;
            n = zb_rd_u32le(r->metadata + at);
            SET_STRING_ELT(md, k, Rf_mkCharLenCE((const char *)r->metadata + at + 4, (int)n, CE_UTF8));
            at += 4 + n;
        }
        Rf_setAttrib(md, R_NamesSymbol, mn);
        rdz_set(out, names, 16, "metadata", md);
        UNPROTECT(2);
    }
    Rf_setAttrib(out, R_NamesSymbol, names);
    UNPROTECT(2);
    return out;
}

SEXP rdz_c_info(SEXP path)
{
    rdz_reader *r;
    rdz_error e;
    SEXP ptr = PROTECT(rdz_reader_handle(&r));
    SEXP out;
    if (rdz_open_source(r, path, &e)) {
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    out = rdz_info_list(r, &e);
    rdz_reader_finalize(ptr);
    UNPROTECT(1);
    return out ? out : rdz_failure(&e);
}

SEXP rdz_generic_write(SEXP x, SEXP synopsis, SEXP path, SEXP settings, SEXP metadata,
                       int fail_after);
SEXP rdz_generic_read(SEXP path, SEXP settings, SEXP select, SEXP window, int *native,
                      double *lo);

SEXP rdz_c_write_generic(SEXP x, SEXP synopsis, SEXP path, SEXP settings, SEXP metadata)
{
    return rdz_generic_write(x, synopsis, path, settings, metadata, -1);
}

/* The one read behind read_rdz(), from one open of the file: select (NULL,
   a character vector, a double vector, or FALSE for one of neither type)
   and window (NULL or c(lo, hi), the rows' span) are resolved against the
   file it opened. list(value, native, lo): whether the file was native, so
   R knows what is left to it (a generic value's selection and rows), and
   the row the value starts after, window[1] when the reader windowed it,
   else 0 (it read the value whole). */
SEXP rdz_c_read(SEXP path, SEXP settings, SEXP select, SEXP window)
{
    int native = 0;
    double lo = 0;
    SEXP value = PROTECT(rdz_generic_read(path, settings, select, window, &native, &lo)), out;
    if (Rf_inherits(value, "rdz_failure")) {
        UNPROTECT(1);
        return value;
    }
    out = PROTECT(Rf_allocVector(VECSXP, 3));
    SET_VECTOR_ELT(out, 0, value);
    SET_VECTOR_ELT(out, 1, Rf_ScalarLogical(native));
    SET_VECTOR_ELT(out, 2, Rf_ScalarReal(lo));
    UNPROTECT(2);
    return out;
}

SEXP rdz_attributes_read(SEXP path, SEXP settings, SEXP steps, SEXP names, int whole_generic,
                         int *native);

/* rdz_attributes()' one read: object (NULL, a list of steps, strings and
   doubles, or FALSE when R found the path malformed), names (NULL or a
   character vector) and allow_full against one open of the file.
   list(native, x): for a native file the named list of the attributes;
   for a generic one its whole value, read only when allow_full (else the
   request failure "attributes_full"). */
SEXP rdz_c_attributes(SEXP path, SEXP settings, SEXP object, SEXP names, SEXP allow_full)
{
    int native = 0, full = Rf_asLogical(allow_full) == TRUE;
    SEXP x = PROTECT(rdz_attributes_read(path, settings, object, names, full, &native)), out;
    if (Rf_inherits(x, "rdz_failure")) {
        UNPROTECT(1);
        return x;
    }
    if (!native && !full) {
        UNPROTECT(1);
        return rdz_request("attributes_full", R_NilValue);
    }
    out = PROTECT(Rf_allocVector(VECSXP, 2));
    SET_VECTOR_ELT(out, 0, Rf_ScalarLogical(native));
    SET_VECTOR_ELT(out, 1, x);
    UNPROTECT(2);
    return out;
}

SEXP rdz_schema_read(SEXP path, SEXP settings, int recursive, int *native);

/* rdz_schema()'s one read: list(native, x). For a native file x is
   list(info, directory, entries, ids, values):
   - info: rdz_info()'s fields (rdz_info_list());
   - directory: list(objects, attributes), each a list of columns: objects
     id, parent, role, type, flags, length, first_child, child_count,
     first_attribute, attribute_count, first_block, block_count,
     stored_bytes, decoded_bytes; attributes owner, kind, name,
     name_object, value_object;
   - entries: per object, the attribute names of each part's object (a
     shared part's target) as rdz_attributes() lists them, else NULL;
   - ids, values: the objects the tree shows (kept containers' names, the
     parts' class and dim), read whole.
   Parts are the root and the children, down to depth 1 unless recursive.
   For a generic file x is list(info). */
SEXP rdz_c_schema(SEXP path, SEXP settings, SEXP recursive)
{
    int native = 0;
    SEXP x = PROTECT(rdz_schema_read(path, settings, Rf_asLogical(recursive) == TRUE, &native)),
         out;
    if (Rf_inherits(x, "rdz_failure")) {
        UNPROTECT(1);
        return x;
    }
    out = PROTECT(Rf_allocVector(VECSXP, 2));
    SET_VECTOR_ELT(out, 0, Rf_ScalarLogical(native));
    SET_VECTOR_ELT(out, 1, x);
    UNPROTECT(2);
    return out;
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
    SEXP out = rdz_generic_write(x, synopsis, path, settings, R_NilValue, Rf_asInteger(blocks));
    UNPROTECT(1);
    return out;
}

/* Files opened so far (rdz_open_source()): read_rdz() opens each once. */
SEXP rdz_test_opens(void)
{
    return Rf_ScalarReal(rdz_opens);
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

/* Every block's stored bytes against its checksum, after the header,
   trailer and directory checks of opening: nothing decompressed, no R
   object built. The block and stored byte counts. */
SEXP rdz_c_verify(SEXP path)
{
    rdz_reader *r;
    rdz_error e;
    uint32_t i;
    double bytes = 0;
    SEXP ptr = PROTECT(rdz_reader_handle(&r)), out;
    if (rdz_open_source(r, path, &e)) {
        rdz_reader_finalize(ptr);
        UNPROTECT(1);
        return rdz_failure(&e);
    }
    for (i = 0; i < r->nblocks; i++) {
        const rdz_block *b = &r->blocks[i];
        if (rdz_reader_read_stored(r, i, &r->scratch, &e)) {
            rdz_reader_finalize(ptr);
            UNPROTECT(1);
            return rdz_failure(&e);
        }
        if (rdz_hash(r->scratch.data, r->scratch.len) != b->checksum) {
            rdz_invalid_block(&e, "checksum mismatch in block %lu", b->sequence);
            rdz_reader_finalize(ptr);
            UNPROTECT(1);
            return rdz_failure(&e);
        }
        bytes += (double)b->stored_len;
        if ((i & 63) == 63) R_CheckUserInterrupt(); /* the reader is ptr's */
    }
    out = PROTECT(Rf_allocVector(REALSXP, 2));
    REAL(out)[0] = (double)r->nblocks;
    REAL(out)[1] = bytes;
    rdz_reader_finalize(ptr);
    UNPROTECT(2);
    return out;
}
