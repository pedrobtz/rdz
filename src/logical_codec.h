#ifndef FASTRDS_LOGICAL_CODEC_H
#define FASTRDS_LOGICAL_CODEC_H

#include <Rinternals.h>

#include <stddef.h>

/* Calculates the number of bytes required for a two-bit logical vector. */
int fastrds_logical_packed_size(R_xlen_t length, size_t *size);

/* Returns 1 on success and 0 if input contains a non-logical value. */
int fastrds_logical_pack(const int *input, R_xlen_t length,
                         unsigned char *packed, size_t packed_size);

/* Returns 1 on success and 0 for invalid codes or non-canonical padding. */
int fastrds_logical_unpack(const unsigned char *packed, size_t packed_size,
                           R_xlen_t length, int *output);

#endif
