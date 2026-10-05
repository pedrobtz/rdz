/*
 * rdz_container.h -- the container reader and writer (container-format.md).
 *
 * The reader validates a file the way the Rust implementation did, check
 * for check and message for message: the file header, the closing trailer,
 * the checksummed directory and its header, every block entry's canonical
 * span, and the schema of the codec the header names. Opening reads only
 * the fixed records and the directory, bounded by the format's limits;
 * rdz_reader_read_block() reads and verifies one data block.
 *
 * The writer is a forward pass: the file header, then each block's header
 * and stored bytes, then the directory and the trailer, into a temporary
 * file that commit renames over the destination (rdz_io.h).
 *
 * Every allocation is a zubin buffer (zb_buf), released by
 * rdz_reader_close() and rdz_writer_discard(), so an R caller that keeps
 * the struct behind an external pointer frees it however R unwinds.
 */
#ifndef RDZ_CONTAINER_H
#define RDZ_CONTAINER_H

#include <zubin/buf.h>

#include "rdz_codec.h"
#include "rdz_format.h"
#include "rdz_io.h"
#include "rdz_records.h"

typedef struct {
    uint16_t container_version, codec_id, codec_version;
    uint32_t block_size;
    uint32_t nobjects, nattributes, nblocks;
    uint64_t payload_bytes, file_bytes, directory_offset;
    const rdz_object *objects;
    const rdz_attribute *attributes;
    const rdz_block *blocks;
    const uint8_t *synopsis; /* into the directory bytes */
    uint32_t synopsis_len;
    zb_buf directory;        /* the directory as stored */
    zb_buf tables;           /* objects, attributes and blocks, decoded */
    zb_buf scratch;          /* stored bytes, for rdz_reader_read_block() */
    zb_buf decoded;          /* a decoded block, for the native readers */
    zb_buf records;          /* decoded character records (rdz_native.c) */
    zb_buf ids;              /* decoded dictionary indices (rdz_native.c) */
    rdz_codec codec;         /* for rdz_reader_read_block() */
    rdz_infile file;
} rdz_reader;

void rdz_reader_init(rdz_reader *r);
/* Opens and validates path. On failure everything is released. */
int rdz_reader_open(rdz_reader *r, const char *path, rdz_error *e);
/* The same over the caller's bytes, which must outlive the reader. */
int rdz_reader_open_memory(rdz_reader *r, const uint8_t *data, size_t n, rdz_error *e);
/* Validates block `index`'s header against its entry and reads its stored
   bytes into `stored`, replacing its contents (its max should allow
   RDZ_MAX_BLOCK_SIZE). File IO: one thread at a time. */
int rdz_reader_read_stored(rdz_reader *r, uint32_t index, zb_buf *stored, rdz_error *e);
/* Verifies stored bytes against the block's checksum and, for a compressed
   block, decompresses them into `out`. A raw block is used where it lies, in
   `stored`, and `out` is untouched. No file IO: a pipeline worker calls it. */
int rdz_block_decode(const rdz_block *b, const zb_buf *stored, zb_buf *out, rdz_codec *codec,
                     rdz_error *e);
/* Both, with the decoded bytes in `out` either way. */
int rdz_reader_read_block(rdz_reader *r, uint32_t index, zb_buf *out, rdz_error *e);
void rdz_reader_close(rdz_reader *r);

typedef struct {
    rdz_outfile out;
    zb_buf entries; /* encoded block entries */
    uint32_t nblocks;
    uint32_t block_size; /* the largest decoded block, recorded in the header */
    int open;
} rdz_writer;

void rdz_writer_init(rdz_writer *w);
/* Creates the temporary file and writes the file header, which records
   block_size as the largest decoded block (RDZ_BLOCK_SIZE normally). */
int rdz_writer_open(rdz_writer *w, const char *path, uint16_t codec_id,
                    uint16_t codec_version, uint32_t block_size, rdz_error *e);
/* Appends one block from its stored bytes, of the given compression, which
   decode to decoded_len bytes; checksum is the stored bytes' XXH3-64. */
int rdz_writer_stored(rdz_writer *w, uint16_t encoding, uint16_t compression,
                      uint64_t logical_count, uint64_t decoded_len, const uint8_t *stored,
                      size_t n, uint64_t checksum, rdz_error *e);
/* Appends one uncompressed block. */
int rdz_writer_block(rdz_writer *w, uint16_t encoding, uint64_t logical_count,
                     const uint8_t *payload, size_t n, rdz_error *e);
/* Writes the directory and trailer and commits the file. */
int rdz_writer_finish(rdz_writer *w, const rdz_object *objects, uint32_t nobjects,
                      const rdz_attribute *attributes, uint32_t nattributes,
                      const uint8_t *synopsis, size_t synopsis_len, rdz_error *e);
/* Removes an uncommitted temporary file and releases everything. */
void rdz_writer_discard(rdz_writer *w);

/* A generic (R serialization) container: payload in 1 MiB raw blocks. */
int rdz_write_generic(const char *path, const uint8_t *payload, size_t n,
                      const uint8_t *synopsis, size_t synopsis_len, rdz_error *e);

#endif /* RDZ_CONTAINER_H */
