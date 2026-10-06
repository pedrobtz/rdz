/*
 * rdz_codec.h -- per-block compression (container-format.md, compression IDs).
 *
 * A block is stored zstd-compressed (compression 1, one zstd frame) only when
 * that saves at least an eighth of its decoded bytes (any saving at level 6
 * and up, the compact preset); otherwise it is
 * stored raw (compression 0), so a block that barely compresses (random
 * logicals, random doubles) costs nothing to decompress on every read. This
 * is writer policy: readers accept any compressed block that is smaller. Blocks
 * under RDZ_COMPRESS_MIN bytes are not tried. Decoding is strict: the frame
 * must be exactly the stored bytes and decode to exactly the decoded length.
 *
 * Threads: a context belongs to one thread at a time; each pipeline worker
 * owns one. zstd is single-threaded here (tools/vendor/zstd-in.c).
 */
#ifndef RDZ_CODEC_H
#define RDZ_CODEC_H

#include <zubin/buf.h>

#include "rdz_format.h"

#define RDZ_COMPRESS_MIN 64u

typedef struct {
    void *cctx;     /* ZSTD_CCtx, created on first use */
    void *dctx;     /* ZSTD_DCtx, created on first use */
    zb_buf scratch; /* a type codec's working memory (logical bitplanes) */
} rdz_codec;

void rdz_codec_init(rdz_codec *c);
void rdz_codec_free(rdz_codec *c);

/* Compresses src at `level` (0 stores raw) into out, replacing out's
   contents. On return *compression is RDZ_COMPRESSION_ZSTD with the frame in
   out, or RDZ_COMPRESSION_NONE with out empty: the caller stores src. */
int rdz_codec_compress(rdz_codec *c, int level, const uint8_t *src, size_t n, zb_buf *out,
                       uint16_t *compression, rdz_error *e);

/* Decodes stored bytes of the given compression into exactly decoded_len
   bytes at dst. For RDZ_COMPRESSION_NONE the caller uses the stored bytes
   and does not call this. */
int rdz_codec_decompress(rdz_codec *c, uint16_t compression, const uint8_t *stored,
                         size_t stored_len, uint8_t *dst, size_t decoded_len, uint32_t sequence,
                         rdz_error *e);

/* zstd's version string, as compiled in. */
const char *rdz_codec_zstd_version(void);

#endif /* RDZ_CODEC_H */
