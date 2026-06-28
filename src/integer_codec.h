#ifndef RDZ_INTEGER_CODEC_H
#define RDZ_INTEGER_CODEC_H

#include <Rinternals.h>

#include <stddef.h>
#include <stdint.h>

#define RDZ_INTEGER_HAS_NA 1
#define RDZ_INTEGER_MAX_PACKED_BITS 8

typedef struct {
    int32_t base;
    uint8_t bits;
    uint8_t flags;
    unsigned char *packed;
    size_t packed_size;
} rdz_integer_encoding_t;

int rdz_integer_encoding_build(SEXP object,
                                   rdz_integer_encoding_t *encoding);
void rdz_integer_encoding_free(rdz_integer_encoding_t *encoding);
int rdz_integer_packed_size(R_xlen_t length, uint8_t bits, size_t *size);
int rdz_integer_decode(const unsigned char *packed, size_t packed_size,
                           int32_t base, uint8_t bits, uint8_t flags,
                           R_xlen_t length, int *output);

#endif
