#include <string.h>

#include "../vendor/zstd/zstd.h"

#include "rdz_codec.h"

/* the bytes of each probe of a large block (rdz_codec_compress()) */
#define RDZ_PROBE ((size_t)32768)

void rdz_codec_init(rdz_codec *c)
{
    c->cctx = NULL;
    c->dctx = NULL;
    zb_buf_alloc(&c->scratch, 0, 0); /* empty: cannot fail */
}

void rdz_codec_free(rdz_codec *c)
{
    ZSTD_freeCCtx((ZSTD_CCtx *)c->cctx);
    ZSTD_freeDCtx((ZSTD_DCtx *)c->dctx);
    zb_buf_release(&c->scratch);
    c->cctx = NULL;
    c->dctx = NULL;
}

int rdz_codec_compress(rdz_codec *c, int level, const uint8_t *src, size_t n, zb_buf *out,
                       uint16_t *compression, rdz_error *e)
{
    size_t bound, got;
    zb_buf_reset(out);
    *compression = RDZ_COMPRESSION_NONE;
    if (level == 0 || n < RDZ_COMPRESS_MIN) return 0;
    if (!c->cctx) {
        c->cctx = ZSTD_createCCtx();
        if (!c->cctx) return rdz_memory(e, "a compression context");
    }
    bound = ZSTD_compressBound(n);
    if (zb_buf_reserve(out, bound)) return rdz_memory(e, "a compressed block");
    /* below level 6 a large block is first tried on three probes, at its
       start, middle and end: when none saves a tenth, the whole is not
       expected to save the eighth that keeps it, and it is stored raw
       without compressing the rest (random integers, random mantissas) */
    if (level < 6 && n >= 8 * RDZ_PROBE) {
        const size_t at[3] = {0, n / 2 - RDZ_PROBE / 2, n - RDZ_PROBE};
        int k, saves = 0;
        for (k = 0; k < 3 && !saves; k++) {
            got = ZSTD_compressCCtx((ZSTD_CCtx *)c->cctx, out->data, bound, src + at[k], RDZ_PROBE,
                                    level);
            if (ZSTD_isError(got)) return rdz_memory(e, "a compressed block");
            saves = got <= RDZ_PROBE - RDZ_PROBE / 10;
        }
        if (!saves) return 0;
    }
    got = ZSTD_compressCCtx((ZSTD_CCtx *)c->cctx, out->data, bound, src, n, level);
    if (ZSTD_isError(got)) return rdz_memory(e, "a compressed block");
    /* worth a decompression on every read only if it saves an eighth; the
       compact levels (6 and up) keep any saving */
    if (level >= 6 ? got < n : got <= n - n / 8) {
        out->len = got;
        *compression = RDZ_COMPRESSION_ZSTD;
    }
    return 0;
}

int rdz_codec_decompress(rdz_codec *c, uint16_t compression, const uint8_t *stored,
                         size_t stored_len, uint8_t *dst, size_t decoded_len, uint32_t sequence,
                         rdz_error *e)
{
    size_t got;
    if (compression != RDZ_COMPRESSION_ZSTD) {
        return rdz_invalid_block(e, "block %lu has an unsupported compression", sequence);
    }
    /* Exactly one frame, filling the stored bytes. */
    got = ZSTD_findFrameCompressedSize(stored, stored_len);
    if (ZSTD_isError(got) || got != stored_len) {
        return rdz_invalid_block(e, "block %lu is not one zstd frame", sequence);
    }
    if (!c->dctx) {
        c->dctx = ZSTD_createDCtx();
        if (!c->dctx) return rdz_memory(e, "a decompression context");
    }
    got = ZSTD_decompressDCtx((ZSTD_DCtx *)c->dctx, dst, decoded_len, stored, stored_len);
    if (ZSTD_isError(got) || got != decoded_len) { /* GUARD: zstd-length */
        return rdz_invalid_block(e, "block %lu does not decompress to its decoded length",
                                 sequence);
    }
    return 0;
}

const char *rdz_codec_zstd_version(void)
{
    return ZSTD_versionString();
}
