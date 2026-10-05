#include <stdlib.h>
#include <string.h>

#include "rdz_container.h"
#include "rdz_pipeline.h"

enum { RDZ_SLOT_FREE = 0, RDZ_SLOT_FILLING, RDZ_SLOT_QUEUED, RDZ_SLOT_BUSY, RDZ_SLOT_DONE };

typedef struct {
    rdz_pipeline *p;
    rdz_codec *codec;
} rdz_worker_arg;

static void rdz_run(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec)
{
    s->failed = 0;
    p->job(p, s, codec);
}

static void *rdz_worker(void *arg)
{
    rdz_worker_arg *a = (rdz_worker_arg *)arg;
    rdz_pipeline *p = a->p;
    pthread_mutex_lock(&p->mu);
    while (!p->stop) {
        rdz_slot *pick = NULL;
        uint32_t i;
        /* the oldest queued slot, so blocks finish roughly in order */
        for (i = 0; i < p->nslots; i++) {
            rdz_slot *s = &p->slots[i];
            if (s->state == RDZ_SLOT_QUEUED && (!pick || s->seq < pick->seq)) pick = s;
        }
        if (!pick) {
            pthread_cond_wait(&p->work, &p->mu);
            continue;
        }
        pick->state = RDZ_SLOT_BUSY;
        pthread_mutex_unlock(&p->mu);
        rdz_run(p, pick, a->codec);
        pthread_mutex_lock(&p->mu);
        pick->state = RDZ_SLOT_DONE;
        pthread_cond_broadcast(&p->done);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

int rdz_pipeline_init(rdz_pipeline *p, int threads, rdz_job_fn job, size_t max, rdz_error *e)
{
    uint32_t i;
    memset(p, 0, sizeof *p);
    p->nthreads = threads < 1 ? 1 : threads;
    p->nslots = (uint32_t)(2 * p->nthreads);
    p->job = job;
    p->slots = (rdz_slot *)calloc(p->nslots, sizeof *p->slots);
    p->ncodecs = p->nthreads + 1;
    p->codecs = (rdz_codec *)calloc((size_t)p->ncodecs, sizeof *p->codecs);
    if (!p->slots || !p->codecs) {
        rdz_pipeline_free(p);
        return rdz_memory(e, "the block pipeline");
    }
    for (i = 0; i < p->nslots; i++) {
        zb_buf_alloc(&p->slots[i].in, 0, max);
        zb_buf_alloc(&p->slots[i].out, 0, max);
    }
    for (i = 0; i < (uint32_t)p->ncodecs; i++) rdz_codec_init(&p->codecs[i]);
    return 0;
}

/* Starts the workers; on any failure runs everything inline instead. */
static void rdz_pipeline_start(rdz_pipeline *p)
{
    rdz_worker_arg *args;
    int i, made = 0;
    if (p->nthreads < 2) return;
    p->threads = (pthread_t *)calloc((size_t)p->nthreads, sizeof *p->threads);
    args = (rdz_worker_arg *)calloc((size_t)p->nthreads, sizeof *args);
    if (!p->threads || !args) goto inline_only;
    if (pthread_mutex_init(&p->mu, NULL)) goto inline_only;
    if (pthread_cond_init(&p->work, NULL)) {
        pthread_mutex_destroy(&p->mu);
        goto inline_only;
    }
    if (pthread_cond_init(&p->done, NULL)) {
        pthread_cond_destroy(&p->work);
        pthread_mutex_destroy(&p->mu);
        goto inline_only;
    }
    p->sync_ok = 1;
    for (i = 0; i < p->nthreads; i++) {
        args[i].p = p;
        args[i].codec = &p->codecs[i + 1];
        if (pthread_create(&p->threads[i], NULL, rdz_worker, &args[i])) break;
        made++;
    }
    if (made < p->nthreads) {
        pthread_mutex_lock(&p->mu);
        p->stop = 1;
        pthread_cond_broadcast(&p->work);
        pthread_mutex_unlock(&p->mu);
        for (i = 0; i < made; i++) pthread_join(p->threads[i], NULL);
        pthread_cond_destroy(&p->done);
        pthread_cond_destroy(&p->work);
        pthread_mutex_destroy(&p->mu);
        p->sync_ok = 0;
        p->stop = 0;
        goto inline_only;
    }
    p->started = made;
    p->worker_args = args;
    return;
inline_only:
    free(p->threads);
    free(args);
    p->threads = NULL;
    p->nthreads = 1;
}

void rdz_pipeline_free(rdz_pipeline *p)
{
    uint32_t i;
    int k;
    if (p->started) {
        pthread_mutex_lock(&p->mu);
        p->stop = 1;
        pthread_cond_broadcast(&p->work);
        pthread_mutex_unlock(&p->mu);
        for (k = 0; k < p->started; k++) pthread_join(p->threads[k], NULL);
        p->started = 0;
    }
    if (p->sync_ok) {
        pthread_cond_destroy(&p->done);
        pthread_cond_destroy(&p->work);
        pthread_mutex_destroy(&p->mu);
        p->sync_ok = 0;
    }
    free(p->threads);
    p->threads = NULL;
    free(p->worker_args);
    p->worker_args = NULL;
    if (p->slots) {
        for (i = 0; i < p->nslots; i++) {
            zb_buf_release(&p->slots[i].in);
            zb_buf_release(&p->slots[i].out);
        }
    }
    free(p->slots);
    p->slots = NULL;
    if (p->codecs) {
        for (k = 0; k < p->ncodecs; k++) rdz_codec_free(&p->codecs[k]);
    }
    free(p->codecs);
    p->codecs = NULL;
}

static int rdz_slot_state(rdz_pipeline *p, rdz_slot *s)
{
    int st;
    if (!p->started) return s->state;
    pthread_mutex_lock(&p->mu);
    st = s->state;
    pthread_mutex_unlock(&p->mu);
    return st;
}

rdz_slot *rdz_pipeline_oldest(rdz_pipeline *p, int wait)
{
    rdz_slot *s;
    int st;
    if (p->next_consume == p->next_submit) return NULL;
    s = &p->slots[p->next_consume % p->nslots];
    if (!p->started) return s->state == RDZ_SLOT_DONE ? s : NULL;
    pthread_mutex_lock(&p->mu);
    while (wait && s->state != RDZ_SLOT_DONE) pthread_cond_wait(&p->done, &p->mu);
    st = s->state;
    pthread_mutex_unlock(&p->mu);
    return st == RDZ_SLOT_DONE ? s : NULL;
}

rdz_slot *rdz_pipeline_next(rdz_pipeline *p, int *must_consume)
{
    rdz_slot *s = &p->slots[p->next_submit % p->nslots];
    if (rdz_slot_state(p, s) != RDZ_SLOT_FREE) {
        /* the ring is full: this slot holds the oldest unconsumed block */
        *must_consume = 1;
        return rdz_pipeline_oldest(p, 1);
    }
    *must_consume = 0;
    s->state = RDZ_SLOT_FILLING;
    s->seq = p->next_submit;
    s->failed = 0;
    zb_buf_reset(&s->in);
    zb_buf_reset(&s->out);
    return s;
}

void rdz_pipeline_unget(rdz_pipeline *p, rdz_slot *s)
{
    (void)p;
    s->state = RDZ_SLOT_FREE; /* FILLING slots are the producer's alone */
}

int rdz_pipeline_submit(rdz_pipeline *p, rdz_slot *s, rdz_error *e)
{
    (void)e;
    p->next_submit++;
    if (!p->started && p->nthreads > 1 && p->next_submit >= 2) rdz_pipeline_start(p);
    if (!p->started) {
        /* one thread, or the first block (the small-input bypass) */
        rdz_run(p, s, &p->codecs[0]);
        s->state = RDZ_SLOT_DONE;
        return 0;
    }
    pthread_mutex_lock(&p->mu);
    s->state = RDZ_SLOT_QUEUED;
    pthread_cond_signal(&p->work);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

void rdz_pipeline_release(rdz_pipeline *p, rdz_slot *s)
{
    if (p->started) pthread_mutex_lock(&p->mu);
    s->state = RDZ_SLOT_FREE;
    if (p->started) pthread_mutex_unlock(&p->mu);
    p->next_consume++;
}

void rdz_job_compress(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec)
{
    s->decoded_len = s->in.len;
    if (rdz_codec_compress(codec, p->level, s->in.data, s->in.len, &s->out, &s->compression,
                           &s->e)) {
        s->failed = 1;
        return;
    }
    s->checksum = s->compression == RDZ_COMPRESSION_NONE ? rdz_hash(s->in.data, s->in.len)
                                                         : rdz_hash(s->out.data, s->out.len);
}

void rdz_job_decode(rdz_pipeline *p, rdz_slot *s, rdz_codec *codec)
{
    const rdz_block *b = (const rdz_block *)s->block;
    (void)p;
    s->compression = b->compression;
    if (rdz_block_decode(b, &s->in, &s->out, codec, &s->e)) s->failed = 1;
}
