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
    rdz_infile file;
} rdz_reader;

void rdz_reader_init(rdz_reader *r);
/* Opens and validates path. On failure everything is released. */
int rdz_reader_open(rdz_reader *r, const char *path, rdz_error *e);
/* The same over the caller's bytes, which must outlive the reader. */
int rdz_reader_open_memory(rdz_reader *r, const uint8_t *data, size_t n, rdz_error *e);
/* Validates block `index`'s header against its entry, reads its stored
   bytes into out (replacing its contents; out's max should allow
   RDZ_MAX_BLOCK_SIZE) and verifies their checksum. */
int rdz_reader_read_block(rdz_reader *r, uint32_t index, zb_buf *out, rdz_error *e);
void rdz_reader_close(rdz_reader *r);

typedef struct {
    rdz_outfile out;
    zb_buf entries; /* encoded block entries */
    uint32_t nblocks;
    int open;
} rdz_writer;

void rdz_writer_init(rdz_writer *w);
/* Creates the temporary file and writes the file header. */
int rdz_writer_open(rdz_writer *w, const char *path, uint16_t codec_id,
                    uint16_t codec_version, rdz_error *e);
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
