/*
 * rdz_alp.h -- double encoding 23, decimal (plan-c Stage J; container-format.md,
 * "Native integer and double representation"): ALP, adaptive lossless
 * floating-point compression (Afroozeh, Kuffó and Boncz, SIGMOD 2024), with
 * a division decode.
 *
 * A double that began as a decimal, x = n * 10^f / 10^e for an integer n,
 * is stored as n: frame of reference or delta, bit-packed, in vectors of
 * 1,024 values that each choose their own (e, f). Values that are not such
 * a decimal (NA, NaN, -0, full-precision values) are exceptions, stored as
 * their bits. Decoding gives the double nearest n * 10^f / 10^e, which is
 * the double R parses or rounds that decimal to: one exact division where
 * double arithmetic is IEEE double (with |n * 10^f| < 2^53, both operands are
 * exact), strtod() where it is x87 extended precision, which would round
 * twice. Threads: pure functions.
 */
#ifndef RDZ_ALP_H
#define RDZ_ALP_H

#include <zubin/buf.h>

#include "rdz_format.h"

#define RDZ_ENCODING_DBL_DECIMAL 23u
#define RDZ_ALP_VECTOR           1024u

/* Encodes v[0, n) as encoding 23 into out when that is promising and smaller
   than `limit` bytes; *used says whether it did (out is then the record). */
int rdz_alp_encode(const double *v, size_t n, size_t limit, zb_buf *out, int *used,
                   rdz_error *e);

int rdz_alp_decode(const uint8_t *enc, size_t len, size_t n, double *out, rdz_error *e);

/* The directory check: whether `len` bytes can hold n values. */
int rdz_alp_length_ok(uint64_t n, uint64_t len);

#endif /* RDZ_ALP_H */
