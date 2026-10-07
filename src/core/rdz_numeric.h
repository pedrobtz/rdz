/*
 * rdz_numeric.h -- the native integer and double codecs (plan-c Stage F;
 * container-format.md, "Native integer and double representation").
 *
 * A block holds up to 262,144 integers or 131,072 doubles (1 MiB raw). Each
 * is stored as the smallest of a few exact records:
 *
 *   integer  10 raw (i32 LE), 11 shuffled raw (4 byte planes), 12 frame of
 *            reference (base + bit-packed offsets; the all-ones code is NA),
 *            13 delta (first + bit-packed deltas over their minimum; no NA),
 *            14 runs (value, end)
 *   double   20 raw (f64 bits LE), 21 shuffled raw (8 byte planes),
 *            22 runs (bits, end), 23 decimals (ALP: rdz_alp.h; when
 *            compressing)
 *
 * Shuffled raw replaces raw when the block will be compressed: the byte
 * planes compress better and decode the same way. Doubles are bit-exact:
 * every NaN payload, NA and -0 survives. Threads: pure functions.
 */
#ifndef RDZ_NUMERIC_H
#define RDZ_NUMERIC_H

#include <zubin/buf.h>

#include "rdz_format.h"

#define RDZ_INT_BLOCK_VALUES ((size_t)262144)
#define RDZ_DBL_BLOCK_VALUES ((size_t)131072)

#define RDZ_ENCODING_INT_RAW     10u
#define RDZ_ENCODING_INT_SHUFFLE 11u
#define RDZ_ENCODING_INT_FOR     12u
#define RDZ_ENCODING_INT_DELTA   13u
#define RDZ_ENCODING_INT_RUNS    14u
#define RDZ_ENCODING_DBL_RAW     20u
#define RDZ_ENCODING_DBL_SHUFFLE 21u
#define RDZ_ENCODING_DBL_RUNS    22u

#define RDZ_INT_FOR_HEADER   8u
#define RDZ_INT_DELTA_HEADER 16u
#define RDZ_RUNS_HEADER      8u
#define RDZ_INT_RUN_RECORD   8u
#define RDZ_DBL_RUN_RECORD   16u

/* Encodes values[0, n) into out as the smallest record (shuffled raw in
   place of raw when `compressing`). */
/* rdz_int_encode(), reporting the block's smallest and largest value other
   than NA (*has_values 0 when every value is NA), from the statistics pass
   it makes anyway. */
int rdz_int_encode_range(const int32_t *values, size_t n, int compressing, zb_buf *out,
                         uint16_t *encoding, int32_t *lo_out, int32_t *hi_out, int *has_values,
                         rdz_error *e);

int rdz_int_encode(const int32_t *values, size_t n, int compressing, zb_buf *out,
                   uint16_t *encoding, rdz_error *e);
int rdz_dbl_encode(const double *values, size_t n, int compressing, zb_buf *out,
                   uint16_t *encoding, rdz_error *e);

/* Whether every value is NA or a factor code from 1 to nlev; one pass in
   vector lanes, meant for a block just decoded, still in cache. */
int rdz_int_codes_ok(const int32_t *v, size_t n, uint64_t nlev);

/* rdz_int_decode() for a factor's codes, which must be NA or 1 to nlev (a
   format error otherwise): checked in the decode's own pass for frame of
   reference, else in a second pass over the block. */
int rdz_factor_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                      uint64_t nlev, rdz_error *e);

int rdz_int_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                   rdz_error *e);
int rdz_dbl_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, double *out,
                   rdz_error *e);

/* Whether a block's decoded length is possible for its encoding and count
   (the directory check; the decoders check the rest). */
int rdz_int_length_ok(uint16_t encoding, uint64_t n, uint64_t len);
int rdz_dbl_length_ok(uint16_t encoding, uint64_t n, uint64_t len);

#endif /* RDZ_NUMERIC_H */
