/* libFuzzer over the R-free container reader (plan-c Stage B): open the
 * input as a container and read and verify every block. Any crash, leak or
 * undefined behaviour is a finding; an error return is the expected outcome
 * for almost every input. */
#include <stddef.h>
#include <stdint.h>

#include "rdz_container.h"

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
    rdz_reader_close(&r);
    return 0;
}
