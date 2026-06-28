#ifndef FASTRDS_INTEGER_CODEC_H
#define FASTRDS_INTEGER_CODEC_H

#include <Rinternals.h>

#include <stddef.h>
#include <stdint.h>

#define FASTRDS_INTEGER_HAS_NA 1
#define FASTRDS_INTEGER_MAX_PACKED_BITS 8

typedef struct {
    int32_t base;
    uint8_t bits;
    uint8_t flags;
    unsigned char *packed;
    size_t packed_size;
} fastrds_integer_encoding_t;

int fastrds_integer_encoding_build(SEXP object,
                                   fastrds_integer_encoding_t *encoding);
void fastrds_integer_encoding_free(fastrds_integer_encoding_t *encoding);
int fastrds_integer_packed_size(R_xlen_t length, uint8_t bits, size_t *size);
int fastrds_integer_decode(const unsigned char *packed, size_t packed_size,
                           int32_t base, uint8_t bits, uint8_t flags,
                           R_xlen_t length, int *output);

#endif
