/*
 * rdz_native.h -- the native codec's files: a logical root with an optional
 * `names` attribute (container-format.md, "Directory header" and "Native
 * logical representation"), written and read as the Rust implementation did.
 *
 * Writing: the root's blocks of up to 65,536 values, then the attribute-name
 * object (one plain record, "names"), then the names' blocks under the
 * dictionary policy; three object entries and one attribute entry. Blocks
 * are stored uncompressed: the logical records are already compact, and
 * general compression of them is evidence-gated (encoding-research.md).
 *
 * Reading uses only buffers the reader owns, so a sink that calls R and
 * jumps leaves nothing that rdz_reader_close() does not free.
 */
#ifndef RDZ_NATIVE_H
#define RDZ_NATIVE_H

#include "rdz_container.h"
#include "rdz_string.h"

/* Writes values[0, n) and, when names is not NULL, its n names. A value or
   name the codec cannot write is RDZ_E_UNSUPPORTED; on any failure no file
   is left behind. */
int rdz_write_native_logical(const char *path, const int32_t *values, size_t n,
                             const rdz_str_source *names, int policy, rdz_error *e);

/* The root's length; RDZ_E_CODEC unless the file is native. */
int rdz_native_length(const rdz_reader *r, size_t *n, rdz_error *e);
/* Reads the root's values into out[0, n). */
int rdz_native_read_logical(rdz_reader *r, int32_t *out, rdz_error *e);

typedef struct {
    void *ctx;
    int (*plain)(void *ctx, const rdz_str *values, size_t count, rdz_error *e);
    int (*entries)(void *ctx, const rdz_str *values, size_t count, rdz_error *e);
    int (*indices)(void *ctx, const uint32_t *ids, size_t count, rdz_error *e);
} rdz_names_sink;

/* Whether the root has names, their length and their dictionary's length,
   from the directory alone. */
int rdz_native_names_info(const rdz_reader *r, int *present, size_t *length,
                          size_t *dictionary_length, rdz_error *e);
/* Reads the names, handing each block to the sink in order. */
int rdz_native_read_names(rdz_reader *r, const rdz_names_sink *sink, rdz_error *e);

#endif /* RDZ_NATIVE_H */
