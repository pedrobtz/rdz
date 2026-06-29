#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include "threading.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
typedef HANDLE rdz_thread_handle_t;
#else
# include <pthread.h>
# include <unistd.h>
typedef pthread_t rdz_thread_handle_t;
# ifdef __linux__
#  include <sched.h>
# endif
#endif

#define RDZ_CGROUP_PATH_SIZE 4096

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

#ifdef __linux__
static long rdz_minimum_positive(long left, long right) {
    if (left < 1) return right;
    if (right < 1) return left;
    return left < right ? left : right;
}

static long rdz_affinity_threads(void) {
    cpu_set_t cpus;
    int count;

    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0) return 0;
    count = CPU_COUNT(&cpus);
    return count > 0 ? (long) count : 0;
}

static int rdz_read_cgroup_path(int unified, char *path, size_t size) {
    FILE *file = fopen("/proc/self/cgroup", "r");
    char line[RDZ_CGROUP_PATH_SIZE];

    if (file == NULL) return 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *first = strchr(line, ':');
        char *second;
        char *controllers;
        char *current;
        size_t length;

        if (first == NULL) continue;
        second = strchr(first + 1, ':');
        if (second == NULL) continue;
        controllers = first + 1;
        *second = '\0';
        current = second + 1;

        if (unified) {
            if (controllers != second) continue;
        } else {
            char *controller = controllers;
            int has_cpu = 0;
            while (controller < second) {
                char *comma = strchr(controller, ',');
                char *end = comma == NULL || comma > second ? second : comma;
                if ((size_t) (end - controller) == 3 &&
                    memcmp(controller, "cpu", 3) == 0) {
                    has_cpu = 1;
                    break;
                }
                controller = end == second ? second : end + 1;
            }
            if (!has_cpu) continue;
        }

        length = strcspn(current, "\r\n");
        if (length == 0 || length >= size || current[0] != '/') continue;
        memcpy(path, current, length);
        path[length] = '\0';
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

static long rdz_quota_threads(long long quota, long long period) {
    long long count;

    if (quota <= 0 || period <= 0) return 0;
    count = quota / period + (quota % period != 0);
    return count > LONG_MAX ? LONG_MAX : (long) count;
}

static long rdz_read_cgroup_v2_limit(const char *file_path) {
    FILE *file = fopen(file_path, "r");
    char quota_text[32];
    long long quota, period;
    char *end;

    if (file == NULL) return 0;
    if (fscanf(file, "%31s %lld", quota_text, &period) != 2) {
        fclose(file);
        return 0;
    }
    fclose(file);
    if (strcmp(quota_text, "max") == 0) return 0;
    quota = strtoll(quota_text, &end, 10);
    if (*quota_text == '\0' || *end != '\0') return 0;
    return rdz_quota_threads(quota, period);
}

static int rdz_read_long_long(const char *file_path, long long *value) {
    FILE *file = fopen(file_path, "r");
    int read;

    if (file == NULL) return 0;
    read = fscanf(file, "%lld", value) == 1;
    fclose(file);
    return read;
}

static long rdz_read_cgroup_v1_limit(const char *directory) {
    char quota_path[RDZ_CGROUP_PATH_SIZE];
    char period_path[RDZ_CGROUP_PATH_SIZE];
    long long quota, period;
    int quota_length = snprintf(
        quota_path, sizeof(quota_path), "%s/cpu.cfs_quota_us", directory);
    int period_length = snprintf(
        period_path, sizeof(period_path), "%s/cpu.cfs_period_us", directory);

    if (quota_length < 0 || (size_t) quota_length >= sizeof(quota_path) ||
        period_length < 0 || (size_t) period_length >= sizeof(period_path) ||
        !rdz_read_long_long(quota_path, &quota) ||
        !rdz_read_long_long(period_path, &period)) return 0;
    return rdz_quota_threads(quota, period);
}

static long rdz_cgroup_v2_threads(void) {
    char cgroup[RDZ_CGROUP_PATH_SIZE];
    char file_path[RDZ_CGROUP_PATH_SIZE];
    long detected = 0;

    if (!rdz_read_cgroup_path(1, cgroup, sizeof(cgroup))) {
        return rdz_read_cgroup_v2_limit("/sys/fs/cgroup/cpu.max");
    }
    for (;;) {
        int length = snprintf(file_path, sizeof(file_path),
                              "/sys/fs/cgroup%s%s", cgroup,
                              strcmp(cgroup, "/") == 0 ? "cpu.max" :
                                                         "/cpu.max");
        char *slash;
        long limit;

        if (length >= 0 && (size_t) length < sizeof(file_path)) {
            limit = rdz_read_cgroup_v2_limit(file_path);
            detected = rdz_minimum_positive(detected, limit);
        }
        if (strcmp(cgroup, "/") == 0) break;
        slash = strrchr(cgroup, '/');
        if (slash == cgroup) cgroup[1] = '\0';
        else *slash = '\0';
    }
    return detected;
}

static long rdz_cgroup_v1_threads(void) {
    static const char *bases[] = {
        "/sys/fs/cgroup/cpu",
        "/sys/fs/cgroup/cpu,cpuacct",
        "/sys/fs/cgroup/cpuacct,cpu"
    };
    char cgroup[RDZ_CGROUP_PATH_SIZE];
    char directory[RDZ_CGROUP_PATH_SIZE];
    long detected = 0;
    size_t base;

    if (!rdz_read_cgroup_path(0, cgroup, sizeof(cgroup))) return 0;
    for (base = 0; base < sizeof(bases) / sizeof(bases[0]); ++base) {
        char current[RDZ_CGROUP_PATH_SIZE];
        memcpy(current, cgroup, strlen(cgroup) + 1);
        for (;;) {
            int length = snprintf(directory, sizeof(directory), "%s%s",
                                  bases[base], current);
            char *slash;
            long limit;

            if (length >= 0 && (size_t) length < sizeof(directory)) {
                limit = rdz_read_cgroup_v1_limit(directory);
                detected = rdz_minimum_positive(detected, limit);
            }
            if (strcmp(current, "/") == 0) break;
            slash = strrchr(current, '/');
            if (slash == current) current[1] = '\0';
            else *slash = '\0';
        }
    }
    return detected;
}

static long rdz_cgroup_threads(void) {
    return rdz_minimum_positive(
        rdz_cgroup_v2_threads(), rdz_cgroup_v1_threads());
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
# ifdef __linux__
    detected = rdz_minimum_positive(detected, rdz_affinity_threads());
    detected = rdz_minimum_positive(detected, rdz_cgroup_threads());
# endif
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
