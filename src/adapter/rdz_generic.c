/*
 * rdz_generic.c -- the generic codec, streamed (plan-c.md section 3.2).
 *
 * Writing: R_Serialize() writes through an R_outpstream whose callback fills
 * a 1 MiB block buffer; each full block goes to the container writer, so the
 * payload never exists whole. Reading: R_Unserialize() reads through an
 * R_inpstream that pulls blocks in order, each verified against its
 * checksum before a byte of it is used. The blocks are those the Rust
 * writer made from serialize(x, NULL, version = 3, xdr = TRUE): full 1 MiB
 * blocks and a final partial one.
 *
 * Both run under R_UnwindProtect(): when R unwinds out of serialization (an
 * error R raises, an interrupt between blocks, a failed block read raised
 * by rdz_raise()) the cleanup removes the temporary file or closes the
 * reader at once, without waiting for the external pointer's finalizer,
 * which remains the backstop. R_CheckUserInterrupt() runs between blocks.
 *
 * R thread only.
 */
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>

#include "../rdz_r.h"

/* ---- writing ------------------------------------------------------------------------ */

typedef struct {
    rdz_writer w;
    zb_buf block;
    SEXP x;
    rdz_error e;
    int failed;
    int fail_after; /* test hook: raise an R error after this many blocks; -1 never */
    uint32_t written;
} rdz_gen_out;

static void rdz_gen_out_release(rdz_gen_out *g)
{
    rdz_writer_discard(&g->w);
    zb_buf_release(&g->block);
}

static void rdz_gen_out_finalize(SEXP ptr)
{
    rdz_gen_out *g = (rdz_gen_out *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_gen_out_release(g);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

/* Hands the buffered bytes to the writer as one block. A write failure is
   kept and later bytes are dropped: serialization runs to its end and the
   failure is returned, classed, by the entry point. */
static void rdz_gen_flush(rdz_gen_out *g, int check_interrupt)
{
    if (!g->failed &&
        rdz_writer_block(&g->w, RDZ_ENCODING_RAW, g->block.len, g->block.data, g->block.len,
                         &g->e)) {
        g->failed = 1;
    }
    zb_buf_reset(&g->block);
    g->written++;
    if (g->fail_after >= 0 && g->written >= (uint32_t)g->fail_after) {
        Rf_error("rdz test hook: failing after %d blocks", g->fail_after);
    }
    if (check_interrupt) R_CheckUserInterrupt();
}

static void rdz_out_bytes(R_outpstream_t stream, void *buf, int n)
{
    rdz_gen_out *g = (rdz_gen_out *)stream->data;
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = n > 0 ? (size_t)n : 0;
    while (left) {
        size_t room = RDZ_BLOCK_SIZE - g->block.len;
        size_t take = left < room ? left : room;
        /* The buffer was allocated at the block size and is never grown. */
        memcpy(g->block.data + g->block.len, p, take);
        g->block.len += take;
        p += take;
        left -= take;
        if (g->block.len == RDZ_BLOCK_SIZE) rdz_gen_flush(g, 1);
    }
}

static void rdz_out_char(R_outpstream_t stream, int c)
{
    unsigned char b = (unsigned char)c;
    rdz_out_bytes(stream, &b, 1);
}

static SEXP rdz_gen_serialize(void *data)
{
    rdz_gen_out *g = (rdz_gen_out *)data;
    struct R_outpstream_st out;
    R_InitOutPStream(&out, (R_pstream_data_t)g, R_pstream_xdr_format, 3, rdz_out_char,
                     rdz_out_bytes, NULL, R_NilValue);
    R_Serialize(g->x, &out);
    if (g->block.len) rdz_gen_flush(g, 0);
    return R_NilValue;
}

static void rdz_gen_out_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_gen_out_finalize((SEXP)data);
}

SEXP rdz_generic_write(SEXP x, SEXP synopsis, SEXP path, int fail_after)
{
    const char *p = rdz_path(path);
    rdz_error e;
    rdz_gen_out *g;
    SEXP ptr, cont;

    if (TYPEOF(synopsis) != RAWSXP) Rf_error("`synopsis` must be a raw vector.");
    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, x));
    R_RegisterCFinalizerEx(ptr, rdz_gen_out_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    g = (rdz_gen_out *)calloc(1, sizeof *g);
    if (!g) Rf_error("rdz could not allocate memory for a writer");
    rdz_writer_init(&g->w);
    zb_buf_init(&g->block);
    R_SetExternalPtrAddr(ptr, g);
    g->x = x;
    g->fail_after = fail_after;

    if (zb_buf_alloc(&g->block, RDZ_BLOCK_SIZE, RDZ_BLOCK_SIZE)) {
        rdz_memory(&e, "a block");
    } else if (rdz_writer_open(&g->w, p, RDZ_CODEC_R_SERIAL_V3, RDZ_R_SERIAL_CODEC_VERSION, &e)) {
        /* e is set */
    } else {
        R_UnwindProtect(rdz_gen_serialize, g, rdz_gen_out_cleanup, ptr, cont);
        if (g->failed) {
            e = g->e;
        } else if (!rdz_writer_finish(&g->w, NULL, 0, NULL, 0, RAW(synopsis),
                                      (size_t)XLENGTH(synopsis), &e)) {
            rdz_gen_out_finalize(ptr);
            UNPROTECT(2);
            return R_NilValue;
        }
    }
    rdz_gen_out_finalize(ptr);
    UNPROTECT(2);
    return rdz_failure(&e);
}

/* ---- reading ------------------------------------------------------------------------ */

typedef struct {
    rdz_reader r;
    zb_buf block;
    size_t pos;
    uint32_t next;
} rdz_gen_in;

static void rdz_gen_in_finalize(SEXP ptr)
{
    rdz_gen_in *g = (rdz_gen_in *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_reader_close(&g->r);
        zb_buf_release(&g->block);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

static void rdz_in_bytes(R_inpstream_t stream, void *buf, int n)
{
    rdz_gen_in *g = (rdz_gen_in *)stream->data;
    uint8_t *p = (uint8_t *)buf;
    size_t left = n > 0 ? (size_t)n : 0;
    rdz_error e;
    while (left) {
        size_t take;
        if (g->pos == g->block.len) {
            if (g->next == g->r.nblocks) {
                rdz_invalid(&e, "the generic payload ends before its object does");
                rdz_raise(&e);
            }
            if (g->next) R_CheckUserInterrupt();
            if (rdz_reader_read_block(&g->r, g->next, &g->block, &e)) rdz_raise(&e);
            g->next++;
            g->pos = 0;
            continue;
        }
        take = g->block.len - g->pos < left ? g->block.len - g->pos : left;
        memcpy(p, g->block.data + g->pos, take);
        g->pos += take;
        p += take;
        left -= take;
    }
}

static int rdz_in_char(R_inpstream_t stream)
{
    unsigned char b;
    rdz_in_bytes(stream, &b, 1);
    return b;
}

static SEXP rdz_gen_unserialize(void *data)
{
    rdz_gen_in *g = (rdz_gen_in *)data;
    struct R_inpstream_st in;
    R_InitInPStream(&in, (R_pstream_data_t)g, R_pstream_any_format, rdz_in_char, rdz_in_bytes,
                    NULL, R_NilValue);
    return R_Unserialize(&in);
}

static void rdz_gen_in_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_gen_in_finalize((SEXP)data);
}

SEXP rdz_c_read(SEXP path)
{
    const char *p = rdz_path(path);
    rdz_error e;
    rdz_gen_in *g;
    SEXP ptr, cont, out;

    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ptr, rdz_gen_in_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    g = (rdz_gen_in *)calloc(1, sizeof *g);
    if (!g) Rf_error("rdz could not allocate memory for a reader");
    rdz_reader_init(&g->r);
    zb_buf_init(&g->block);
    R_SetExternalPtrAddr(ptr, g);

    if (rdz_reader_open(&g->r, p, &e)) {
        rdz_gen_in_finalize(ptr);
        UNPROTECT(2);
        return rdz_failure(&e);
    }
    if (g->r.codec_id == RDZ_CODEC_NATIVE_V1) {
        /* Native codecs are read by the Rust oracle until Stage E. */
        rdz_gen_in_finalize(ptr);
        out = PROTECT(Rf_mkString("native_v1"));
        Rf_setAttrib(out, R_ClassSymbol, PROTECT(Rf_mkString("rdz_native_codec")));
        UNPROTECT(4);
        return out;
    }
    if (zb_buf_alloc(&g->block, 0, (size_t)RDZ_MAX_BLOCK_SIZE)) {
        rdz_memory(&e, "a block");
        rdz_gen_in_finalize(ptr);
        UNPROTECT(2);
        return rdz_failure(&e);
    }
    out = PROTECT(R_UnwindProtect(rdz_gen_unserialize, g, rdz_gen_in_cleanup, ptr, cont));
    rdz_gen_in_finalize(ptr);
    UNPROTECT(3);
    return out;
}

/* ---- the synopsis ----------------------------------------------------------------- */

/* The length the synopsis records, read without dispatch: -1 for a type it
   does not summarise (as the Rust implementation did). */
SEXP rdz_c_root_length(SEXP x)
{
    switch (TYPEOF(x)) {
    case NILSXP:
        return Rf_ScalarReal(0);
    case LGLSXP: case INTSXP: case REALSXP: case RAWSXP: case STRSXP: case VECSXP:
        return Rf_ScalarReal((double)XLENGTH(x));
    default:
        return Rf_ScalarReal(-1);
    }
}
