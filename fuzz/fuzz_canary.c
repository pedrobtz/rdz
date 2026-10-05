/* The fuzz gate's canary: the same reader, trapping as soon as it accepts a
 * container, which the seed corpus (the reference fixtures) guarantees.
 * tools/run-fuzz requires this to crash before it trusts the real target: a
 * gate counts once it has been seen to fail. */
#include <stddef.h>
#include <stdint.h>

#include "rdz_container.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    rdz_reader r;
    rdz_error e;
    if (rdz_reader_open_memory(&r, data, size, &e) == 0) {
        rdz_reader_close(&r);
        __builtin_trap();
    }
    return 0;
}
