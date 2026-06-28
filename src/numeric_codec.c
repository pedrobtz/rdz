#include "numeric_codec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REAL_SAMPLE_SIZE 1024
#define REAL_SAMPLE_UNIQUE_LIMIT 64
#define REAL_HASH_SIZE 512
#define REAL_LINEAR_LOOKUP_MAX 8

typedef struct {
    uint64_t keys[REAL_HASH_SIZE];
    uint16_t indexes[REAL_HASH_SIZE];
    unsigned char used[REAL_HASH_SIZE];
} real_hash_t;

static uint64_t real_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double real_from_bits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double sequence_value(double base, double delta, R_xlen_t index) {
    return base + (double) index * delta;
}

static uint64_t mix64(uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value;
}

static size_t hash_slot(uint64_t bits) {
    return (size_t) mix64(bits) & (REAL_HASH_SIZE - 1);
}

static int sample_dictionary(SEXP object, uint64_t *dictionary,
                             uint16_t *dictionary_size) {
    R_xlen_t length = XLENGTH(object);
    R_xlen_t sample_size = length < REAL_SAMPLE_SIZE ? length : REAL_SAMPLE_SIZE;
    R_xlen_t i;
    const double *values = REAL_RO(object);

    *dictionary_size = 0;
    if (length < 64) return 0;
    for (i = 0; i < sample_size; ++i) {
        R_xlen_t at = sample_size == length ? i :
            (R_xlen_t) (((uint64_t) i * (uint64_t) length) /
                        (uint64_t) sample_size);
        uint64_t bits = real_bits(values[at]);
        uint16_t j;
        for (j = 0; j < *dictionary_size; ++j) {
            if (dictionary[j] == bits) break;
        }
        if (j == *dictionary_size) {
            if (*dictionary_size == REAL_SAMPLE_UNIQUE_LIMIT) return 0;
            dictionary[*dictionary_size] = bits;
            ++*dictionary_size;
        }
    }
    return 1;
}

static int hash_find_or_add(real_hash_t *hash, uint64_t bits,
                            uint16_t *dictionary_size,
                            uint64_t *dictionary, uint16_t *index) {
    size_t slot;

    if (*dictionary_size <= REAL_LINEAR_LOOKUP_MAX) {
        uint16_t i;
        for (i = 0; i < *dictionary_size; ++i) {
            if (dictionary[i] == bits) {
                *index = i;
                return 1;
            }
        }
        if (*dictionary_size == FASTRDS_REAL_DICTIONARY_MAX) return 0;
        *index = *dictionary_size;
        dictionary[*dictionary_size] = bits;
        ++*dictionary_size;
        slot = hash_slot(bits);
        while (hash->used[slot]) slot = (slot + 1) & (REAL_HASH_SIZE - 1);
        hash->used[slot] = 1;
        hash->keys[slot] = bits;
        hash->indexes[slot] = *index;
        return 1;
    }

    slot = hash_slot(bits);
    while (hash->used[slot] && hash->keys[slot] != bits) {
        slot = (slot + 1) & (REAL_HASH_SIZE - 1);
    }
    if (hash->used[slot]) {
        *index = hash->indexes[slot];
        return 1;
    }
    if (*dictionary_size == FASTRDS_REAL_DICTIONARY_MAX) return 0;
    *index = *dictionary_size;
    dictionary[*dictionary_size] = bits;
    ++*dictionary_size;
    hash->used[slot] = 1;
    hash->keys[slot] = bits;
    hash->indexes[slot] = *index;
    return 1;
}

static uint8_t bits_for_count(uint16_t count) {
    uint16_t maximum;
    uint8_t bits = 0;
    if (count <= 1) return 0;
    maximum = (uint16_t) (count - 1);
    while (maximum != 0) {
        ++bits;
        maximum >>= 1;
    }
    return bits;
}

int fastrds_real_dictionary_packed_size(R_xlen_t length, uint8_t bits,
                                        size_t *size) {
    uint64_t total_bits;
    if (bits > 8 || length < 0 ||
        (uint64_t) length > (UINT64_MAX - 7) / (bits == 0 ? 1 : bits)) {
        return 0;
    }
    total_bits = (uint64_t) length * bits;
    if ((total_bits + 7) / 8 > SIZE_MAX) return 0;
    *size = (size_t) ((total_bits + 7) / 8);
    return 1;
}

static int sampled_dictionary_find(const uint64_t *dictionary, uint16_t count,
                                   uint64_t bits, uint16_t *index) {
    uint16_t i;

    if (count > 0 && dictionary[0] == bits) { *index = 0; return 1; }
    if (count > 1 && dictionary[1] == bits) { *index = 1; return 1; }
    if (count > 2 && dictionary[2] == bits) { *index = 2; return 1; }
    if (count > 3 && dictionary[3] == bits) { *index = 3; return 1; }
    for (i = 4; i < count; ++i) {
        if (dictionary[i] == bits) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

/* Returns 1 when the sampled dictionary covered the full vector, 0 when a
 * later value requires the general builder, and -1 on allocation failure. */
static int sampled_dictionary_build(
    SEXP object, const uint64_t *sampled, uint16_t sampled_size,
    fastrds_real_dictionary_t *dictionary
) {
    const double *values = REAL_RO(object);
    R_xlen_t length = XLENGTH(object), i;
    size_t raw_size, encoded_size, position = 0;
    uint64_t accumulator = 0;
    unsigned accumulator_bits = 0;

    dictionary->count = sampled_size;
    memcpy(dictionary->values, sampled,
           (size_t) sampled_size * sizeof(uint64_t));
    dictionary->bits = bits_for_count(sampled_size);
    if (!fastrds_real_dictionary_packed_size(
            length, dictionary->bits, &dictionary->packed_size) ||
        (uint64_t) length > SIZE_MAX / sizeof(double)) {
        memset(dictionary, 0, sizeof(*dictionary));
        return 0;
    }
    raw_size = (size_t) length * sizeof(double);
    encoded_size = 1 + sizeof(uint16_t) +
        (size_t) sampled_size * sizeof(uint64_t) + dictionary->packed_size;
    if (encoded_size >= raw_size) {
        memset(dictionary, 0, sizeof(*dictionary));
        return 0;
    }
    if (dictionary->packed_size != 0) {
        dictionary->packed = (unsigned char *) calloc(dictionary->packed_size, 1);
        if (dictionary->packed == NULL) return -1;
    }

    for (i = 0; i < length; ++i) {
        uint64_t bits = real_bits(values[i]);
        uint16_t index;
        if (!sampled_dictionary_find(sampled, sampled_size, bits, &index)) {
            free(dictionary->packed);
            memset(dictionary, 0, sizeof(*dictionary));
            return 0;
        }
        accumulator |= (uint64_t) index << accumulator_bits;
        accumulator_bits += dictionary->bits;
        while (accumulator_bits >= 8) {
            dictionary->packed[position++] = (unsigned char) accumulator;
            accumulator >>= 8;
            accumulator_bits -= 8;
        }
    }
    if (accumulator_bits != 0) {
        dictionary->packed[position++] = (unsigned char) accumulator;
    }
    if (position != dictionary->packed_size) {
        free(dictionary->packed);
        memset(dictionary, 0, sizeof(*dictionary));
        return -1;
    }
    return 1;
}

int fastrds_real_dictionary_build(SEXP object,
                                  fastrds_real_dictionary_t *dictionary) {
    real_hash_t hash;
    uint64_t sampled[REAL_SAMPLE_UNIQUE_LIMIT];
    uint16_t sampled_size;
    const double *values;
    R_xlen_t length, i;
    size_t raw_size, encoded_size;
    unsigned char *indexes;
    uint64_t accumulator = 0;
    unsigned accumulator_bits = 0;
    size_t position = 0;

    memset(dictionary, 0, sizeof(*dictionary));
    if (TYPEOF(object) != REALSXP ||
        !sample_dictionary(object, sampled, &sampled_size)) return 0;
    if (sampled_size <= REAL_LINEAR_LOOKUP_MAX) {
        int result = sampled_dictionary_build(
            object, sampled, sampled_size, dictionary);
        if (result != 0) return result;
    }
    length = XLENGTH(object);
    values = REAL_RO(object);
    memset(&hash, 0, sizeof(hash));
    if ((uint64_t) length > SIZE_MAX) return 0;
    indexes = (unsigned char *) malloc(length == 0 ? 1 : (size_t) length);
    if (indexes == NULL) return -1;

    for (i = 0; i < length; ++i) {
        uint16_t index;
        if (!hash_find_or_add(&hash, real_bits(values[i]), &dictionary->count,
                              dictionary->values, &index)) {
            free(indexes);
            memset(dictionary, 0, sizeof(*dictionary));
            return 0;
        }
        indexes[i] = (unsigned char) index;
    }
    dictionary->bits = bits_for_count(dictionary->count);
    if (!fastrds_real_dictionary_packed_size(length, dictionary->bits,
                                             &dictionary->packed_size) ||
        (uint64_t) length > SIZE_MAX / sizeof(double)) {
        free(indexes);
        memset(dictionary, 0, sizeof(*dictionary));
        return 0;
    }
    raw_size = (size_t) length * sizeof(double);
    encoded_size = 1 + sizeof(uint16_t) +
        (size_t) dictionary->count * sizeof(uint64_t) + dictionary->packed_size;
    if (encoded_size >= raw_size) {
        free(indexes);
        memset(dictionary, 0, sizeof(*dictionary));
        return 0;
    }
    if (dictionary->packed_size != 0) {
        dictionary->packed = (unsigned char *) calloc(dictionary->packed_size, 1);
        if (dictionary->packed == NULL) {
            free(indexes);
            return -1;
        }
    }

    for (i = 0; i < length; ++i) {
        uint16_t index = indexes[i];
        accumulator |= (uint64_t) index << accumulator_bits;
        accumulator_bits += dictionary->bits;
        while (accumulator_bits >= 8) {
            dictionary->packed[position++] = (unsigned char) accumulator;
            accumulator >>= 8;
            accumulator_bits -= 8;
        }
    }
    if (accumulator_bits != 0) {
        dictionary->packed[position++] = (unsigned char) accumulator;
    }
    free(indexes);
    if (position != dictionary->packed_size) {
        fastrds_real_dictionary_free(dictionary);
        return -1;
    }
    return 1;
}

void fastrds_real_dictionary_free(fastrds_real_dictionary_t *dictionary) {
    free(dictionary->packed);
    dictionary->packed = NULL;
    dictionary->packed_size = 0;
}

int fastrds_real_dictionary_decode(
    const unsigned char *packed, size_t packed_size, uint8_t bits,
    const uint64_t *dictionary, uint16_t dictionary_size,
    R_xlen_t length, double *output
) {
    uint64_t accumulator = 0;
    uint64_t mask = bits == 0 ? 0 : (UINT64_C(1) << bits) - 1;
    unsigned accumulator_bits = 0;
    size_t position = 0;
    R_xlen_t i;

    if (dictionary_size == 0 || bits > 8 ||
        (dictionary_size == 1 && bits != 0) ||
        (dictionary_size > 1 &&
         (bits == 0 || (UINT16_C(1) << bits) < dictionary_size))) {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        uint16_t index;
        while (accumulator_bits < bits) {
            if (position >= packed_size) return 0;
            accumulator |= (uint64_t) packed[position++] << accumulator_bits;
            accumulator_bits += 8;
        }
        index = bits == 0 ? 0 : (uint16_t) (accumulator & mask);
        if (bits != 0) {
            accumulator >>= bits;
            accumulator_bits -= bits;
        }
        if (index >= dictionary_size) return 0;
        memcpy(output + i, dictionary + index, sizeof(uint64_t));
    }
    return position == packed_size;
}

int fastrds_real_sequence_build(SEXP object,
                                fastrds_real_sequence_t *sequence) {
    const double *values;
    double base, delta;
    R_xlen_t length, i;

    memset(sequence, 0, sizeof(*sequence));
    if (TYPEOF(object) != REALSXP || XLENGTH(object) < 3) return 0;
    length = XLENGTH(object);
    values = REAL_RO(object);
    base = values[0];
    delta = values[1] - values[0];
    if (!isfinite(base) || !isfinite(delta)) return 0;

    for (i = 2; i < length; ++i) {
        if (real_bits(sequence_value(base, delta, i)) != real_bits(values[i])) {
            return 0;
        }
    }
    sequence->base = real_bits(base);
    sequence->delta = real_bits(delta);
    return 1;
}

int fastrds_real_sequence_decode(const fastrds_real_sequence_t *sequence,
                                 R_xlen_t length, double *output) {
    double base = real_from_bits(sequence->base);
    double delta = real_from_bits(sequence->delta);
    R_xlen_t i;
    if (!isfinite(base) || !isfinite(delta)) return 0;
    for (i = 0; i < length; ++i) {
        output[i] = sequence_value(base, delta, i);
    }
    return 1;
}

void fastrds_real_xor_encode_block(const double *input, size_t length,
                                   uint64_t *previous,
                                   unsigned char *output) {
    size_t i;

    for (i = 0; i < length; ++i) {
        uint64_t bits = real_bits(input[i]);
        uint64_t delta = bits ^ *previous;
        *previous = bits;
        output[i] = (unsigned char) (delta >> 56);
        output[length + i] = (unsigned char) (delta >> 48);
        output[2 * length + i] = (unsigned char) (delta >> 40);
        output[3 * length + i] = (unsigned char) (delta >> 32);
        output[4 * length + i] = (unsigned char) (delta >> 24);
        output[5 * length + i] = (unsigned char) (delta >> 16);
        output[6 * length + i] = (unsigned char) (delta >> 8);
        output[7 * length + i] = (unsigned char) delta;
    }
}

void fastrds_real_xor_decode_block(const unsigned char *input, size_t length,
                                   uint64_t *previous, double *output) {
    size_t i;

    for (i = 0; i < length; ++i) {
        uint64_t delta = (uint64_t) input[i] << 56 |
            (uint64_t) input[length + i] << 48 |
            (uint64_t) input[2 * length + i] << 40 |
            (uint64_t) input[3 * length + i] << 32 |
            (uint64_t) input[4 * length + i] << 24 |
            (uint64_t) input[5 * length + i] << 16 |
            (uint64_t) input[6 * length + i] << 8 |
            (uint64_t) input[7 * length + i];
        uint64_t bits;
        bits = delta ^ *previous;
        *previous = bits;
        memcpy(output + i, &bits, sizeof(bits));
    }
}
