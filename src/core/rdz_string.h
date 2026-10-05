/*
 * rdz_string.h -- character records (container-format.md, "Character
 * dictionary blocks"), ported from the Rust implementation byte for byte.
 *
 * A record is a one-byte tag (0 NA, 1 native, 2 UTF-8, 3 Latin-1, 4 bytes),
 * a u32 length and the bytes; blocks of records hold at most 1 MiB. The
 * dictionary policies write each distinct value once as an entry record and
 * the elements as indices (1, 2 or 4 bytes over a base) in blocks of 65,536:
 * "block" restarts the identity map every such chunk, "global" keeps one for
 * the vector (until 2^22 entries, after which the rest is written plain),
 * and "auto" is global when a sample suggests fewer than 75% distinct
 * values, plain otherwise. Identity is the caller's key (in R, the CHARSXP
 * address, which R's string cache makes unique per value); keys are never
 * written.
 *
 * Threads: pure functions of their arguments; the source and the emitter
 * are called on the caller's thread.
 */
#ifndef RDZ_STRING_H
#define RDZ_STRING_H

#include <zubin/buf.h>

#include "rdz_format.h"

enum { RDZ_STR_NA = 0, RDZ_STR_NATIVE = 1, RDZ_STR_UTF8 = 2, RDZ_STR_LATIN1 = 3, RDZ_STR_BYTES = 4 };
enum { RDZ_DICT_PLAIN = 0, RDZ_DICT_BLOCK = 1, RDZ_DICT_GLOBAL = 2, RDZ_DICT_AUTO = 3 };

#define RDZ_STRING_RECORD_HEADER 5u
#define RDZ_DICT_CHUNK_VALUES ((size_t)65536)

typedef struct {
    uint8_t tag; /* RDZ_STR_* */
    const uint8_t *bytes;
    size_t len;
} rdz_str;

typedef struct {
    size_t n;
    void *ctx;
    uintptr_t (*key)(void *ctx, size_t i);
    /* fills *out; an error (RDZ_E_UNSUPPORTED for a value the codec cannot
       write) stops the encoding */
    int (*value)(void *ctx, size_t i, rdz_str *out, rdz_error *e);
} rdz_str_source;

/* Called once per block, in file order. */
typedef int (*rdz_emit_fn)(void *ctx, uint16_t encoding, uint64_t count, const uint8_t *data,
                           size_t n, rdz_error *e);

int rdz_string_encode(const rdz_str_source *src, int policy, rdz_emit_fn emit, void *ectx,
                      rdz_error *e);

/* Decodes the `count` records of a plain or dictionary-entry block into
   out[0, count), pointing into `block`. */
int rdz_string_decode_records(const uint8_t *block, size_t len, size_t count, rdz_str *out,
                              rdz_error *e);

/* Decodes an index block into absolute ids, each below dictionary_len. */
int rdz_string_decode_indices(const uint8_t *block, size_t len, size_t count,
                              size_t dictionary_len, uint32_t *out, rdz_error *e);

#endif /* RDZ_STRING_H */
