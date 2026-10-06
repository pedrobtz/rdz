/*
 * rdz_pipeline.h -- the bounded, ordered block pipeline (performance.md).
 *
 * A ring of `nslots` slots, each holding one block's input and output
 * buffers. The producer (the R thread) takes the slot for the next sequence
 * number, fills its input and submits it; a worker runs the job on it; the
 * producer consumes finished slots strictly in sequence order. When the slot
 * the producer needs next still holds an unconsumed block, the producer
 * waits for that block and consumes it first, so at most `nslots` blocks are
 * in flight and memory is bounded by nslots times two block buffers.
 *
 * With one thread no worker exists and every job runs inline, at submission:
 * the deterministic one-thread path. With more, the workers start on the
 * second submission, so a one-block object never pays for them (the
 * small-input bypass). Output is the same either way: jobs are pure
 * functions of their slot.
 *
 * Errors: the first failing slot's error is reported when it is consumed,
 * in order; rdz_pipeline_free() stops and joins the workers whatever state
 * the pipeline is in, so it is safe from an unwind cleanup.
 *
 * Threads: one producer thread calls everything here; workers touch only
 * their slot and their own codec context, never R.
 */
#ifndef RDZ_PIPELINE_H
#define RDZ_PIPELINE_H

#include <pthread.h>

#include <zubin/buf.h>

#include "rdz_codec.h"
#include "rdz_format.h"

typedef struct {
    zb_buf in;
    zb_buf out;
    uint64_t seq;
    int state; /* RDZ_SLOT_* in rdz_pipeline.c */
    int failed;
    rdz_error e;
    /* the job's parameters and results */
    uint32_t index;
    uint16_t encoding, compression;
    uint64_t logical_count, decoded_len, checksum;
    const zb_buf *result; /* the job's output: the stored bytes written, or
                             the decoded bytes read (in or out) */
    const void *block;    /* a read job's rdz_block */
    uint16_t vtype;       /* a vector job's RDZ_TYPE_*; RDZ_TYPE_CHARACTER: the
                             slot holds a packed string block to compress */
} rdz_slot;

struct rdz_pipeline;
/* The job a worker runs on a slot, with the worker's own codec context. */
typedef void (*rdz_job_fn)(struct rdz_pipeline *p, rdz_slot *s, rdz_codec *codec);

typedef struct rdz_pipeline {
    rdz_slot *slots;
    uint32_t nslots;
    int nthreads;        /* workers to run; 1 runs jobs inline */
    int started;         /* workers running */
    pthread_t *threads;
    void *worker_args;   /* the workers' arguments, alive while they run */
    rdz_codec *codecs;   /* one per worker, plus one for the inline path */
    int ncodecs;
    pthread_mutex_t mu;
    pthread_cond_t work; /* a slot was submitted, or stop */
    pthread_cond_t done; /* a slot finished */
    int stop;
    int sync_ok;         /* mu and the conditions exist */
    uint64_t next_submit;
    uint64_t next_consume;
    rdz_job_fn job;
    int level;           /* for compression jobs */

} rdz_pipeline;

/* threads >= 1. Buffers grow to max. slots: 2 * threads, at least 2, but no
   more than hold 1 GiB of blocks of `block` bytes (the largest block the
   caller expects: a file's declared block size, or the writer's). */
int rdz_pipeline_init(rdz_pipeline *p, int threads, rdz_job_fn job, size_t max, size_t block,
                      rdz_error *e);
void rdz_pipeline_free(rdz_pipeline *p);

/* The slot for the next sequence number, empty and free. If it still holds
   an unconsumed block, returns that block's slot instead with *must_consume
   set: consume it (rdz_pipeline_release() it) and ask again. */
rdz_slot *rdz_pipeline_next(rdz_pipeline *p, int *must_consume);
/* Gives back a slot from rdz_pipeline_next() that was never submitted. */
void rdz_pipeline_unget(rdz_pipeline *p, rdz_slot *s);
/* Hands a filled slot to the workers (or runs it inline). */
int rdz_pipeline_submit(rdz_pipeline *p, rdz_slot *s, rdz_error *e);
/* The oldest submitted, unconsumed slot once it is finished; NULL when none
   is outstanding. With wait 0, NULL also when it is not finished yet. */
rdz_slot *rdz_pipeline_oldest(rdz_pipeline *p, int wait);
/* Marks the oldest slot consumed. */
void rdz_pipeline_release(rdz_pipeline *p, rdz_slot *s);

/* The jobs. compress: in holds bytes, compressed or not. decode: in holds a
   block's stored bytes, verified and decompressed. vector: in holds
   logical_count values of s->vtype, encoded as a record and compressed (a
   character slot holds an encoded string block, compressed as it is). */
void rdz_job_compress(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec);
void rdz_job_decode(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec);
void rdz_job_vector(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec);

#endif /* RDZ_PIPELINE_H */
