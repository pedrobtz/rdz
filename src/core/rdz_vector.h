/*
 * rdz_vector.h -- native vectors through the block pipeline: a logical,
 * integer or double root with an optional `names` attribute.
 *
 * The caller's thread copies each block's values into a pipeline slot (so
 * workers never touch the caller's memory), workers encode, compress and
 * checksum them, and the caller's thread writes finished blocks in order;
 * reading mirrors it, with the caller's thread decoding each verified
 * record into the output. `tick` runs on the caller's thread between
 * blocks and may not return (an R interrupt): every resource is in the
 * rdz_vec struct, which the caller frees with rdz_vec_free() from any
 * unwind cleanup.
 *
 * Logical blocks are written uncompressed whatever the level, as the Rust
 * implementation wrote them; integer and double blocks are compressed at
 * `level` (0: raw).
 */
#ifndef RDZ_VECTOR_H
#define RDZ_VECTOR_H

#include "rdz_native.h"
#include "rdz_pipeline.h"

typedef void (*rdz_tick_fn)(void *ctx);

typedef struct {
    rdz_writer w;
    rdz_pipeline pipe;
    int have_pipe;
} rdz_vec;

void rdz_vec_init(rdz_vec *v);
/* Joins any workers and removes an uncommitted temporary file. */
void rdz_vec_free(rdz_vec *v);

/* What to write. */
typedef struct {
    uint16_t type;                  /* RDZ_TYPE_* */
    size_t n;
    const void *values;             /* logical, integer and factor codes: int32;
                                       double: double */
    const rdz_str_source *strings;  /* a character root's values */
    const rdz_str_source *levels;   /* a factor's levels */
    int ordered;                    /* a factor: c("ordered", "factor") */
    const rdz_str_source *names;    /* NULL: no names */
    int policy, level, threads;
    rdz_tick_fn tick;
    void *tick_ctx;
} rdz_vec_spec;

int rdz_vec_write(rdz_vec *v, const char *path, const rdz_vec_spec *spec, rdz_error *e);

/* The root's type tag and length, for a native file. */
int rdz_vec_shape(const rdz_reader *r, uint16_t *type, size_t *n, rdz_error *e);
/* Reads a string object's blocks through v's pipeline into the sink. */
int rdz_vec_read_strings(rdz_vec *v, rdz_reader *r, uint32_t object, const rdz_names_sink *sink,
                         int threads, rdz_tick_fn tick, void *tick_ctx, rdz_error *e);
/* Reads the root's values into out (n elements: int32 for logical,
   integer and factor codes, double for double). */
int rdz_vec_read(rdz_vec *v, rdz_reader *r, void *out, int threads, rdz_tick_fn tick,
                 void *tick_ctx, rdz_error *e);

/* Values per block for a type. */
size_t rdz_vec_block_values(uint16_t type);

#endif /* RDZ_VECTOR_H */
