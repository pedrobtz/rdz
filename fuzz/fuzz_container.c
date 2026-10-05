/* libFuzzer over the R-free container reader (plan-c Stage B): open the
 * input as a container and read and verify every block. Any crash, leak or
 * undefined behaviour is a finding; an error return is the expected outcome
 * for almost every input. */
#include <stddef.h>
#include <stdint.h>

#include <stdlib.h>

#include "rdz_container.h"
#include "rdz_native.h"

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
        size_t n;
        if (rdz_native_length(&r, &n, &e) == 0 && n <= ((size_t)1 << 20)) {
            int32_t *values = (int32_t *)malloc((n ? n : 1) * sizeof *values);
            rdz_names_sink sink;
            sink.ctx = NULL;
            sink.plain = count_plain;
            sink.entries = count_plain;
            sink.indices = count_ids;
            if (values && rdz_native_read_logical(&r, values, &e) == 0) {
                rdz_native_read_names(&r, &sink, &e);
            }
            free(values);
        }
    }
    rdz_reader_close(&r);
    return 0;
}
