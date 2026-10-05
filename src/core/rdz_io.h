/*
 * rdz_io.h -- files, with 64-bit offsets on every platform.
 *
 * Paths are the bytes R hands over: UTF-8 on Windows, where they are opened
 * through the wide-character API, and the native encoding elsewhere.
 *
 * An output file is written to a temporary file beside its destination and
 * renamed over it on commit, keeping an existing destination's permission
 * bits (POSIX). On a platform whose rename cannot replace a file, the
 * destination is first moved to a backup, which is removed after the rename
 * or moved back if it fails. Nothing is synced: the replacement is atomic
 * for other processes, not durable across a crash.
 *
 * Threads: an rdz_infile or rdz_outfile belongs to one thread at a time.
 * Creating temporary names uses a process-wide counter, so rdz_outfile_open
 * is called from one thread only (the R thread).
 */
#ifndef RDZ_IO_H
#define RDZ_IO_H

#include <stdio.h>

#include "rdz_format.h"

typedef struct {
    FILE *fp;
    const uint8_t *mem; /* instead of fp: bytes the caller keeps alive */
    uint64_t size;
} rdz_infile;

/* Opens path for reading and records its size. */
int rdz_infile_open(rdz_infile *f, const char *path, rdz_error *e);
/* Reads from the caller's n bytes instead (the fuzz targets). */
void rdz_infile_open_memory(rdz_infile *f, const uint8_t *data, size_t n);
/* Reads exactly n bytes at offset; a short read is "file is truncated". */
int rdz_infile_read_at(rdz_infile *f, uint64_t offset, void *buf, size_t n, rdz_error *e);
void rdz_infile_close(rdz_infile *f);

typedef struct {
    FILE *fp;
    char *destination;
    char *temporary;
    uint64_t position;
    int have_mode;
    unsigned mode;
} rdz_outfile;

void rdz_outfile_init(rdz_outfile *f);
/* Creates the temporary file beside path. */
int rdz_outfile_open(rdz_outfile *f, const char *path, rdz_error *e);
int rdz_outfile_write(rdz_outfile *f, const void *data, size_t n, rdz_error *e);
/* Closes the temporary file and renames it over the destination. */
int rdz_outfile_commit(rdz_outfile *f, rdz_error *e);
/* Closes and removes the temporary file if it was not committed; releases
   everything. Safe to call more than once and after a commit. */
void rdz_outfile_discard(rdz_outfile *f);

#endif /* RDZ_IO_H */
