/*
 * rdz_logical.h -- the native logical codec (container-format.md, "Native
 * logical representation"), ported from the Rust implementation byte for
 * byte.
 *
 * A block of at most 65,536 values is classified into TRUE and NA bitplanes
 * (FALSE is neither) with counts and the number of runs, and stored as the
 * smallest of five records: constant (3), modal-default bitplanes (4), sparse
 * patches (5), run ends (6) or a short period (7). Encoding 1, the two-bit
 * reference, is read but never written. The classifier has a scalar
 * reference and AVX2 and NEON kernels chosen at run time; they produce the
 * same planes, so the bytes never depend on the CPU.
 *
 * Values are R's logical representation: 0, 1 and NA (INT32_MIN); any other
 * int is an error. Threads: pure functions of their arguments.
 */
#ifndef RDZ_LOGICAL_H
#define RDZ_LOGICAL_H

#include <zubin/buf.h>

#include "rdz_format.h"

#define RDZ_LOGICAL_NA INT32_MIN

/* Encodes values[0, n) into out (replacing its contents) as the smallest
   record, which *encoding names. n <= RDZ_LOGICAL_BLOCK_VALUES. scratch
   holds the planes and is reused across calls. */
int rdz_logical_encode(const int32_t *values, size_t n, zb_buf *out, uint16_t *encoding,
                       zb_buf *scratch, rdz_error *e);

/* Decodes one block of `count` values into out. */
int rdz_logical_decode(const uint8_t *encoded, size_t len, uint16_t encoding, size_t count,
                       int32_t *out, rdz_error *e);

/* Which classifier runs here: "avx2", "neon" or "scalar". */
const char *rdz_logical_kernel(void);
/* Forces the scalar classifier (nonzero) or restores the dispatch (zero),
   so tests can compare kernels. */
void rdz_logical_force_scalar(int on);

#endif /* RDZ_LOGICAL_H */
