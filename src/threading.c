#include "threading.h"

#include <stdlib.h>

#ifdef _WIN32
# include <windows.h>
typedef HANDLE rdz_thread_handle_t;
#else
# include <pthread.h>
# include <unistd.h>
typedef pthread_t rdz_thread_handle_t;
#endif

typedef struct {
    rdz_parallel_worker_t worker;
    void *data;
    size_t worker_index;
    size_t worker_count;
} rdz_thread_argument_t;

#ifdef _WIN32
static DWORD WINAPI rdz_thread_entry(LPVOID pointer) {
    rdz_thread_argument_t *argument = (rdz_thread_argument_t *) pointer;
    argument->worker(argument->worker_index, argument->worker_count,
                     argument->data);
    return 0;
}
#else
static void *rdz_thread_entry(void *pointer) {
    rdz_thread_argument_t *argument = (rdz_thread_argument_t *) pointer;
    argument->worker(argument->worker_index, argument->worker_count,
                     argument->data);
    return NULL;
}
#endif

int rdz_resolve_threads(int requested_threads) {
    long detected = 1;

    if (requested_threads > 0) {
        return requested_threads > RDZ_MAX_THREADS ?
            RDZ_MAX_THREADS : requested_threads;
    }
#ifdef _WIN32
    {
        SYSTEM_INFO information;
        GetSystemInfo(&information);
        detected = (long) information.dwNumberOfProcessors;
    }
#else
    detected = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (detected < 1) detected = 1;
    if (detected > RDZ_MAX_THREADS) detected = RDZ_MAX_THREADS;
    return (int) detected;
}

int rdz_parallel_run(int requested_threads, size_t task_count,
                     rdz_parallel_worker_t worker, void *data) {
    size_t worker_count, i;
    int concurrent = 1;
    rdz_thread_handle_t *handles;
    rdz_thread_argument_t *arguments;
    unsigned char *created;

    if (task_count == 0) return 0;
    worker_count = (size_t) rdz_resolve_threads(requested_threads);
    if (worker_count > task_count) worker_count = task_count;
    if (worker_count <= 1) {
        worker(0, 1, data);
        return 1;
    }

    handles = (rdz_thread_handle_t *) calloc(
        worker_count - 1, sizeof(*handles));
    arguments = (rdz_thread_argument_t *) calloc(
        worker_count, sizeof(*arguments));
    created = (unsigned char *) calloc(worker_count, 1);
    if (handles == NULL || arguments == NULL || created == NULL) {
        free(handles);
        free(arguments);
        free(created);
        worker(0, 1, data);
        return 1;
    }

    for (i = 0; i < worker_count; ++i) {
        arguments[i].worker = worker;
        arguments[i].data = data;
        arguments[i].worker_index = i;
        arguments[i].worker_count = worker_count;
    }
    for (i = 1; i < worker_count; ++i) {
#ifdef _WIN32
        handles[i - 1] = CreateThread(
            NULL, 0, rdz_thread_entry, &arguments[i], 0, NULL);
        if (handles[i - 1] != NULL) {
            created[i] = 1;
            ++concurrent;
        }
#else
        if (pthread_create(&handles[i - 1], NULL, rdz_thread_entry,
                           &arguments[i]) == 0) {
            created[i] = 1;
            ++concurrent;
        }
#endif
    }

    worker(0, worker_count, data);
    for (i = 1; i < worker_count; ++i) {
        if (!created[i]) continue;
#ifdef _WIN32
        (void) WaitForSingleObject(handles[i - 1], INFINITE);
        (void) CloseHandle(handles[i - 1]);
#else
        (void) pthread_join(handles[i - 1], NULL);
#endif
    }
    /* A failed thread creation leaves its static task partition untouched.
       Complete those partitions serially after all background work finishes. */
    for (i = 1; i < worker_count; ++i) {
        if (!created[i]) worker(i, worker_count, data);
    }

    free(handles);
    free(arguments);
    free(created);
    return concurrent;
}
