/*
 * rdz_generic.c -- the generic codec, streamed through the block pipeline
 * (plan-c.md sections 3.2 and 4; performance.md).
 *
 * Writing: R_Serialize() writes through an R_outpstream whose callback fills
 * the current pipeline slot; each full block is submitted to the workers,
 * which compress it (raw when that is not smaller) and checksum it, and the
 * R thread writes finished blocks to the file strictly in order. Reading:
 * the R thread reads blocks' stored bytes ahead into the pipeline, the
 * workers verify and decompress them, and R_Unserialize() reads them in
 * order through an R_inpstream. The payload never exists whole, and memory
 * is bounded by the pipeline's slots (rdz_pipeline.h).
 *
 * Both run under R_UnwindProtect(): when R unwinds out of serialization (an
 * error R raises, an interrupt between blocks, a failed block raised by
 * rdz_raise()) the cleanup stops and joins the workers and removes the
 * temporary file or closes the reader at once; the external pointer's
 * finalizer is the backstop. Only this, the R thread, calls R.
 */
#include <stdlib.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>

#include <zufast/hash.h>

#include "../core/rdz_pipeline.h"
#include "../rdz_r.h"

#include "../core/rdz_vector.h"

SEXP rdz_native_read_r(rdz_reader *opened, int threads, SEXP select, rdz_error *e,
                       int *failed);

/* settings: c(level, threads, block_size); level 0 stores raw, block_size 0
   is the format's 1 MiB. */
typedef struct {
    int level;
    int threads;
    uint32_t block_size;
} rdz_settings;

static rdz_settings rdz_settings_of(SEXP settings)
{
    rdz_settings out;
    out.level = 0;
    out.threads = 1;
    out.block_size = RDZ_BLOCK_SIZE;
    if (TYPEOF(settings) != INTSXP || XLENGTH(settings) != 3) {
        Rf_error("`settings` must be an integer vector of length 3.");
    }
    out.level = INTEGER(settings)[0];
    out.threads = INTEGER(settings)[1] < 1 ? 1 : INTEGER(settings)[1];
    if (out.threads > 256) out.threads = 256;
    if (INTEGER(settings)[2] > 0) out.block_size = (uint32_t)INTEGER(settings)[2];
    return out;
}

/* ---- writing ------------------------------------------------------------------------ */

typedef struct {
    rdz_writer w;
    rdz_pipeline pipe;
    rdz_slot *cur; /* the slot being filled */
    uint32_t block_size;
    SEXP x;
    rdz_error e;
    int failed;
    int fail_after; /* test hook: raise an R error after this many blocks; -1 never */
    uint32_t submitted;
} rdz_gen_out;

static void rdz_gen_out_finalize(SEXP ptr)
{
    rdz_gen_out *g = (rdz_gen_out *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_pipeline_free(&g->pipe); /* joins the workers first */
        rdz_writer_discard(&g->w);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

/* Writes one finished slot to the file and frees it. */
static void rdz_gen_consume(rdz_gen_out *g, rdz_slot *s)
{
    if (!g->failed) {
        if (s->failed) {
            g->e = s->e;
            g->failed = 1;
        } else {
            if (rdz_writer_stored(&g->w, RDZ_ENCODING_RAW, s->compression, s->decoded_len,
                                  s->decoded_len, s->result->data, s->result->len, s->checksum,
                                  &g->e)) {
                g->failed = 1;
            }
        }
    }
    rdz_pipeline_release(&g->pipe, s);
}

/* The next empty slot, writing out whatever must leave first. */
static rdz_slot *rdz_gen_next(rdz_gen_out *g)
{
    for (;;) {
        int must = 0;
        rdz_slot *s = rdz_pipeline_next(&g->pipe, &must);
        if (!must) return s;
        rdz_gen_consume(g, s);
    }
}

/* Submits the current slot, writes out every block already finished, in
   order, and checks for an interrupt between blocks. A failure is kept and
   later bytes are dropped: serialization runs to its end and the failure is
   returned, classed, by the entry point. */
static void rdz_gen_flush(rdz_gen_out *g, int last)
{
    rdz_slot *s;
    rdz_pipeline_submit(&g->pipe, g->cur, &g->e);
    g->cur = NULL;
    g->submitted++;
    while ((s = rdz_pipeline_oldest(&g->pipe, 0)) != NULL) rdz_gen_consume(g, s);
    if (g->fail_after >= 0 && g->submitted >= (uint32_t)g->fail_after) {
        Rf_error("rdz test hook: failing after %d blocks", g->fail_after);
    }
    if (!last) {
        R_CheckUserInterrupt();
        g->cur = rdz_gen_next(g);
    }
}

static void rdz_out_bytes(R_outpstream_t stream, void *buf, int n)
{
    rdz_gen_out *g = (rdz_gen_out *)stream->data;
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = n > 0 ? (size_t)n : 0;
    while (left) {
        zb_buf *in = &g->cur->in;
        size_t room = g->block_size - in->len;
        size_t take = left < room ? left : room;
        uint8_t *slot = zb_put_raw(in, take);
        if (!slot) {
            if (!g->failed) {
                rdz_memory(&g->e, "a block");
                g->failed = 1;
            }
            return;
        }
        memcpy(slot, p, take);
        p += take;
        left -= take;
        if (in->len == g->block_size) rdz_gen_flush(g, 0);
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
    rdz_slot *s;
    R_InitOutPStream(&out, (R_pstream_data_t)g, R_pstream_xdr_format, 3, rdz_out_char,
                     rdz_out_bytes, NULL, R_NilValue);
    R_Serialize(g->x, &out);
    if (g->cur->in.len) {
        rdz_gen_flush(g, 1);
    } else {
        /* the payload ended on a block boundary: no empty trailing block */
        rdz_pipeline_unget(&g->pipe, g->cur);
        g->cur = NULL;
    }
    while ((s = rdz_pipeline_oldest(&g->pipe, 1)) != NULL) rdz_gen_consume(g, s);
    return R_NilValue;
}

static void rdz_gen_out_cleanup(void *data, Rboolean jump)
{
    if (jump) rdz_gen_out_finalize((SEXP)data);
}

/* ---- the content hash of a generic value -------------------------------------------- */

/* R serialization version 2 (ALTREP written as its values) past its 14-byte
   header (which names the writing R's version), as digest hashes R values,
   streamed into XXH3-128: nothing allocated, so an R error mid-stream loses
   nothing. */
typedef struct {
    zuf_hasher h;
    size_t skip;
} rdz_hash_stream;

static void rdz_hash_out_bytes(R_outpstream_t stream, void *buf, int n)
{
    rdz_hash_stream *hs = (rdz_hash_stream *)stream->data;
    const unsigned char *p = (const unsigned char *)buf;
    size_t len = (size_t)n;
    if (hs->skip) {
        size_t drop = hs->skip < len ? hs->skip : len;
        hs->skip -= drop;
        p += drop;
        len -= drop;
    }
    if (len) zuf_hasher_update(&hs->h, p, len);
}

static void rdz_hash_out_char(R_outpstream_t stream, int c)
{
    unsigned char b = (unsigned char)c;
    rdz_hash_out_bytes(stream, &b, 1);
}

void rdz_generic_hash(SEXP x, uint8_t out[16])
{
    static const unsigned char magic[8] = {'R', 'D', 'Z', 'H', 1, 'G', 0, 0};
    rdz_hash_stream hs;
    struct R_outpstream_st stream;
    zuf_digest128 d;
    zuf_hasher_init(&hs.h, 0);
    zuf_hasher_update(&hs.h, magic, sizeof magic);
    hs.skip = 14; /* "X\n", then the format, R's and the reader's minimum versions */
    R_InitOutPStream(&stream, (R_pstream_data_t)&hs, R_pstream_xdr_format, 2, rdz_hash_out_char,
                     rdz_hash_out_bytes, NULL, R_NilValue);
    R_Serialize(x, &stream);
    d = zuf_hasher_digest128(&hs.h);
    zb_wr_u64le(out, d.low);
    zb_wr_u64le(out + 8, d.high);
}

SEXP rdz_c_hash_generic(SEXP x)
{
    uint8_t digest[16];
    rdz_generic_hash(x, digest);
    return rdz_hash_text(digest);
}

SEXP rdz_generic_write(SEXP x, SEXP synopsis, SEXP path, SEXP settings, int fail_after)
{
    const char *p = rdz_path(path);
    rdz_settings set = rdz_settings_of(settings);
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
    R_SetExternalPtrAddr(ptr, g);
    g->x = x;
    g->fail_after = fail_after;
    g->block_size = set.block_size;

    if (rdz_pipeline_init(&g->pipe, set.threads, rdz_job_compress, (size_t)RDZ_MAX_BLOCK_SIZE,
                          &e)) {
        /* e is set */
    } else if (rdz_writer_open(&g->w, p, RDZ_CODEC_R_SERIAL_V3, RDZ_R_SERIAL_CODEC_VERSION,
                               set.block_size, &e)) {
        /* e is set */
    } else {
        /* the content hash first: the directory the write ends with holds it */
        rdz_generic_hash(x, g->w.content_hash);
        g->w.hash_scheme = RDZ_CONTENT_HASH_V1;
        g->pipe.level = set.level;
        g->cur = rdz_gen_next(g);
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
    rdz_pipeline pipe;
    rdz_vec vec;         /* a native file's pipeline */
    int threads;
    rdz_error e;
    int failed;
    rdz_slot *cur;       /* the slot being read from */
    const zb_buf *bytes; /* its decoded bytes: the slot's in or out */
    size_t pos;
    uint32_t next_read;  /* the next block whose stored bytes to read ahead */
} rdz_gen_in;

static void rdz_gen_in_finalize(SEXP ptr)
{
    rdz_gen_in *g = (rdz_gen_in *)R_ExternalPtrAddr(ptr);
    if (g) {
        rdz_pipeline_free(&g->pipe);
        rdz_vec_free(&g->vec);
        rdz_reader_close(&g->r);
        free(g);
        R_ClearExternalPtr(ptr);
    }
}

/* Reads stored bytes ahead into every free slot, handing each to a worker.
   File IO stays on this thread. */
static void rdz_gen_read_ahead(rdz_gen_in *g)
{
    rdz_error e;
    while (g->next_read < g->r.nblocks &&
           g->pipe.next_submit - g->pipe.next_consume < g->pipe.nslots) {
        int must = 0;
        rdz_slot *s = rdz_pipeline_next(&g->pipe, &must);
        if (must) return;
        if (rdz_reader_read_stored(&g->r, g->next_read, &s->in, &e)) {
            rdz_pipeline_unget(&g->pipe, s);
            rdz_raise(&e);
        }
        s->block = &g->r.blocks[g->next_read];
        s->index = g->next_read;
        g->next_read++;
        rdz_pipeline_submit(&g->pipe, s, &e);
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
        if (!g->cur || g->pos == g->bytes->len) {
            if (g->cur) {
                rdz_pipeline_release(&g->pipe, g->cur);
                g->cur = NULL;
                R_CheckUserInterrupt();
            }
            rdz_gen_read_ahead(g);
            g->cur = rdz_pipeline_oldest(&g->pipe, 1);
            if (!g->cur) {
                rdz_invalid(&e, "the generic payload ends before its object does");
                rdz_raise(&e);
            }
            if (g->cur->failed) rdz_raise(&g->cur->e);
            g->bytes = g->cur->result;
            g->pos = 0;
            continue;
        }
        take = g->bytes->len - g->pos < left ? g->bytes->len - g->pos : left;
        memcpy(p, g->bytes->data + g->pos, take);
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

/* select: R_NilValue, or 0-based children of a native list or data frame
   root to read alone (R selects from a generic root after reading it).
   *native: whether the file was native. */
SEXP rdz_generic_read(SEXP path, SEXP settings, SEXP select, int *native)
{
    const char *p = rdz_path(path);
    rdz_settings set = rdz_settings_of(settings);
    rdz_error e;
    rdz_gen_in *g;
    SEXP ptr, cont, out;

    ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ptr, rdz_gen_in_finalize, TRUE);
    cont = PROTECT(R_MakeUnwindCont());
    g = (rdz_gen_in *)calloc(1, sizeof *g);
    if (!g) Rf_error("rdz could not allocate memory for a reader");
    rdz_reader_init(&g->r);
    rdz_vec_init(&g->vec);
    g->threads = set.threads;
    R_SetExternalPtrAddr(ptr, g);

    if (rdz_reader_open(&g->r, p, &e)) {
        rdz_gen_in_finalize(ptr);
        UNPROTECT(2);
        return rdz_failure(&e);
    }
    *native = g->r.codec_id == RDZ_CODEC_NATIVE_V1;
    if (g->r.codec_id == RDZ_CODEC_NATIVE_V1) {
        int failed;
        /* the native reader takes the open reader over */
        out = PROTECT(rdz_native_read_r(&g->r, set.threads, select, &e, &failed));
        rdz_gen_in_finalize(ptr);
        UNPROTECT(3);
        return failed ? rdz_failure(&e) : out;
    }
    /* A one-block file needs no workers. */
    if (rdz_pipeline_init(&g->pipe, g->r.nblocks > 1 ? set.threads : 1, rdz_job_decode,
                          (size_t)RDZ_MAX_BLOCK_SIZE, &e)) {
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
