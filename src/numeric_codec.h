#ifndef RDZ_NUMERIC_CODEC_H
#define RDZ_NUMERIC_CODEC_H

#include <Rinternals.h>

#include <stddef.h>
#include <stdint.h>

#define RDZ_REAL_DICTIONARY_MAX 256
#define RDZ_REAL_XOR_BLOCK_VALUES 8192

typedef struct {
    uint8_t bits;
    uint16_t count;
    uint64_t values[RDZ_REAL_DICTIONARY_MAX];
    unsigned char *packed;
    size_t packed_size;
} rdz_real_dictionary_t;

typedef struct {
    uint64_t base;
    uint64_t delta;
} rdz_real_sequence_t;

/* Returns 1 when dictionary encoding was built, 0 when direct storage is
 * preferable, and -1 on allocation failure. */
int rdz_real_dictionary_build(SEXP object,
                                  rdz_real_dictionary_t *dictionary);

void rdz_real_dictionary_free(rdz_real_dictionary_t *dictionary);

int rdz_real_dictionary_packed_size(R_xlen_t length, uint8_t bits,
                                        size_t *size);

/* Returns 1 on success and 0 for malformed packed indexes. */
int rdz_real_dictionary_decode(
    const unsigned char *packed, size_t packed_size, uint8_t bits,
    const uint64_t *dictionary, uint16_t dictionary_size,
    R_xlen_t length, double *output
);

/* Selects only sequences that can be reconstructed bit-for-bit. */
int rdz_real_sequence_build(SEXP object,
                                rdz_real_sequence_t *sequence);

/* Returns 1 on success and 0 for malformed sequence parameters. */
int rdz_real_sequence_decode(const rdz_real_sequence_t *sequence,
                                 R_xlen_t length, double *output);

/* XORs each value's bit pattern with its predecessor and byte-transposes the
 * result so homogeneous byte lanes can be compressed independently. */
void rdz_real_xor_encode_block(const double *input, size_t length,
                                   uint64_t *previous,
                                   unsigned char *output);

void rdz_real_xor_decode_block(const unsigned char *input, size_t length,
                                   uint64_t *previous, double *output);

#endif
