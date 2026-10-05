#include <string.h>

#include "../vendor/zstd/zstd.h"

#include "rdz_codec.h"

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
    got = ZSTD_compressCCtx((ZSTD_CCtx *)c->cctx, out->data, bound, src, n, level);
    if (ZSTD_isError(got)) return rdz_memory(e, "a compressed block");
    if (got < n) {
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
    if (ZSTD_isError(got) || got != decoded_len) {
        return rdz_invalid_block(e, "block %lu does not decompress to its decoded length",
                                 sequence);
    }
    return 0;
}

const char *rdz_codec_zstd_version(void)
{
    return ZSTD_versionString();
}
