#ifndef RDZ_THREADING_H
#define RDZ_THREADING_H

#include <stddef.h>

#define RDZ_MAX_THREADS 64

typedef void (*rdz_parallel_worker_t)(size_t worker_index,
                                      size_t worker_count,
                                      void *data);

int rdz_resolve_threads(int requested_threads);

/* Runs one worker on the calling thread and up to worker_count - 1 workers on
 * background threads. Returns the number of workers that ran concurrently. */
int rdz_parallel_run(int requested_threads, size_t task_count,
                     rdz_parallel_worker_t worker, void *data);

#endif
