/* libFuzzer over the R-free container reader (plan-c Stage B): open the
 * input as a container and read and verify every block. Any crash, leak or
 * undefined behaviour is a finding; an error return is the expected outcome
 * for almost every input. */
#include <stddef.h>
#include <stdint.h>

#include <stdlib.h>

#include "rdz_container.h"
#include "rdz_native.h"
#include "rdz_graph.h"
#include "rdz_vector.h"

/* Every numeric object's values in one arena, at precomputed offsets. */
typedef struct {
    double *arena;
    size_t *offset;
    rdz_names_sink sink;
} fuzz_graph;

static void *graph_values(void *ctx, uint32_t object)
{
    fuzz_graph *g = (fuzz_graph *)ctx;
    return g->arena + g->offset[object];
}

static const rdz_names_sink *graph_strings(void *ctx, uint32_t object)
{
    (void)object;
    return &((fuzz_graph *)ctx)->sink;
}

static int count_plain(void *ctx, const rdz_str *v, size_t n, rdz_error *e)
{
    (void)ctx; (void)v; (void)n; (void)e;
    return 0;
}

static int count_ids(void *ctx, const uint32_t *ids, size_t n, rdz_error *e)
{
    (void)ctx; (void)ids; (void)n; (void)e;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    rdz_reader r;
    rdz_error e;
    zb_buf block;
    uint32_t i;
    if (rdz_reader_open_memory(&r, data, size, &e)) return 0;
    if (zb_buf_alloc(&block, 0, (size_t)RDZ_MAX_BLOCK_SIZE) == ZB_OK) {
        for (i = 0; i < r.nblocks; i++) {
            if (rdz_reader_read_block(&r, i, &block, &e)) break;
        }
        zb_buf_release(&block);
    }
    /* a native file's values and names, when small enough to decode */
    {
        uint16_t type;
        size_t n;
        if (rdz_vec_shape(&r, &type, &n, &e) == 0 && n <= ((size_t)1 << 20)) {
            void *values = malloc((n ? n : 1) * sizeof(double));
            rdz_names_sink sink;
            rdz_vec v;
            sink.ctx = NULL;
            sink.plain = count_plain;
            sink.entries = count_plain;
            sink.indices = count_ids;
            rdz_vec_init(&v);
            if (type == RDZ_TYPE_CHARACTER) {
                rdz_vec_read_strings(&v, &r, 0, &sink, 2, NULL, NULL, &e);
            } else if (values && rdz_vec_read(&v, &r, values, 2, NULL, NULL, &e) == 0) {
                if (type == RDZ_TYPE_FACTOR) rdz_vec_read_strings(&v, &r, 1, &sink, 2, NULL, NULL, &e);
                rdz_native_read_names(&r, &sink, &e);
            }
            rdz_vec_free(&v);
            free(values);
        }
    }
    /* the whole object graph, when its values are small enough to hold */
    if (r.codec_id == RDZ_CODEC_NATIVE_V1) {
        size_t total = 0;
        uint32_t k;
        fuzz_graph g;
        g.offset = (size_t *)calloc(r.nobjects ? r.nobjects : 1, sizeof(size_t));
        for (k = 0; g.offset && k < r.nobjects && total <= ((size_t)1 << 22); k++) {
            g.offset[k] = total;
            if (r.objects[k].type_tag != RDZ_TYPE_CHARACTER) total += (size_t)r.objects[k].logical_len;
        }
        if (g.offset && total <= ((size_t)1 << 22)) {
            g.arena = (double *)malloc((total ? total : 1) * sizeof(double));
            g.sink.ctx = NULL;
            g.sink.plain = count_plain;
            g.sink.entries = count_plain;
            g.sink.indices = count_ids;
            if (g.arena) {
                rdz_graph_sinks sinks;
                rdz_vec v;
                sinks.ctx = &g;
                sinks.values = graph_values;
                sinks.strings = graph_strings;
                rdz_vec_init(&v);
                rdz_graph_read(&v, &r, &sinks, 2, NULL, NULL, &e);
                rdz_vec_free(&v);
                /* and a selection: objects picked by the input's last byte */
                {
                    uint8_t *want = (uint8_t *)calloc(r.nobjects ? r.nobjects : 1, 1);
                    uint8_t pick = size ? data[size - 1] : 0;
                    if (want) {
                        for (k = 0; k < r.nobjects; k++) want[k] = (uint8_t)((pick >> (k % 8)) & 1u);
                        rdz_vec_init(&v);
                        rdz_graph_read_some(&v, &r, &sinks, want, 2, NULL, NULL, &e);
                        rdz_vec_free(&v);
                        free(want);
                    }
                }
                /* and windows: each object's elements [lo, hi), from the input */
                {
                    rdz_window *win = (rdz_window *)calloc(r.nobjects ? r.nobjects : 1, sizeof *win);
                    uint8_t pick = size > 1 ? data[size - 2] : 0;
                    if (win) {
                        for (k = 0; k < r.nobjects; k++) {
                            uint64_t len = r.objects[k].logical_len;
                            win[k].on = (pick >> (k % 8)) & 1u;
                            win[k].lo = len ? (uint64_t)pick * 977u % len : 0;
                            win[k].hi = win[k].lo + (len - win[k].lo) / 2;
                        }
                        rdz_vec_init(&v);
                        rdz_graph_read_window(&v, &r, &sinks, NULL, win, 2, NULL, NULL, &e);
                        rdz_vec_free(&v);
                        free(win);
                    }
                }
            }
            free(g.arena);
        }
        free(g.offset);
    }
    rdz_reader_close(&r);
    return 0;
}
