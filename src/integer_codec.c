#include "integer_codec.h"

#include <R.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define INTEGER_SAMPLE_SIZE 1024

static uint8_t bits_for_maximum(uint64_t maximum) {
    uint8_t bits = 0;
    while (maximum != 0) {
        ++bits;
        maximum >>= 1;
    }
    return bits;
}

static uint8_t range_bits(int minimum, int maximum, int has_na) {
    uint64_t range = (uint64_t) ((int64_t) maximum - (int64_t) minimum);
    return bits_for_maximum(range + (has_na != 0));
}

static uint64_t integer_code(int value, int minimum, int has_na) {
    if (value == NA_INTEGER) return 0;
    return (uint64_t) ((int64_t) value - (int64_t) minimum) +
        (has_na != 0);
}

static int sample_is_packable(SEXP object) {
    R_xlen_t length = XLENGTH(object);
    R_xlen_t sample_size = length < INTEGER_SAMPLE_SIZE ?
        length : INTEGER_SAMPLE_SIZE;
    const int *values = INTEGER_RO(object);
    int minimum = 0, maximum = 0, has_na = 0, has_value = 0;
    R_xlen_t i;

    if (length == 0) return 0;
    for (i = 0; i < sample_size; ++i) {
        R_xlen_t at = sample_size == length ? i :
            (R_xlen_t) (((uint64_t) i * (uint64_t) length) /
                        (uint64_t) sample_size);
        int value = values[at];
        if (value == NA_INTEGER) {
            has_na = 1;
        } else {
            if (!has_value) {
                minimum = maximum = value;
                has_value = 1;
            } else {
                if (value < minimum) minimum = value;
                if (value > maximum) maximum = value;
            }
        }
    }
    if (!has_value) return 1;
    return range_bits(minimum, maximum, has_na) <=
        RDZ_INTEGER_MAX_PACKED_BITS;
}

int rdz_integer_packed_size(R_xlen_t length, uint8_t bits, size_t *size) {
    uint64_t total_bits;
    if (length < 0 || size == NULL ||
        bits > RDZ_INTEGER_MAX_PACKED_BITS ||
        (uint64_t) length > (UINT64_MAX - 7) / (bits == 0 ? 1 : bits)) {
        return 0;
    }
    total_bits = (uint64_t) length * bits;
    if ((total_bits + 7) / 8 > SIZE_MAX) return 0;
    *size = (size_t) ((total_bits + 7) / 8);
    return 1;
}

int rdz_integer_encoding_build(SEXP object,
                                   rdz_integer_encoding_t *encoding) {
    const int *values;
    R_xlen_t length, i;
    int minimum = 0, maximum = 0, has_na = 0, has_value = 0;
    size_t raw_size, encoded_size, position = 0;
    uint64_t accumulator = 0;
    unsigned accumulator_bits = 0;

    memset(encoding, 0, sizeof(*encoding));
    if (TYPEOF(object) != INTSXP || !sample_is_packable(object)) return 0;
    length = XLENGTH(object);
    values = INTEGER_RO(object);

    for (i = 0; i < length; ++i) {
        int value = values[i];
        if (value == NA_INTEGER) {
            has_na = 1;
        } else {
            if (!has_value) {
                minimum = maximum = value;
                has_value = 1;
            } else {
                if (value < minimum) minimum = value;
                if (value > maximum) maximum = value;
            }
        }
    }
    if (!has_value) {
        minimum = 0;
        maximum = 0;
    }
    encoding->base = (int32_t) minimum;
    encoding->flags = has_na ? RDZ_INTEGER_HAS_NA : 0;
    encoding->bits = has_value ? range_bits(minimum, maximum, has_na) : 0;
    if (encoding->bits > RDZ_INTEGER_MAX_PACKED_BITS ||
        !rdz_integer_packed_size(length, encoding->bits,
                                     &encoding->packed_size) ||
        (uint64_t) length > SIZE_MAX / sizeof(int)) {
        memset(encoding, 0, sizeof(*encoding));
        return 0;
    }
    raw_size = (size_t) length * sizeof(int);
    encoded_size = sizeof(int32_t) + sizeof(uint8_t) + sizeof(uint8_t) +
        encoding->packed_size;
    if (encoded_size >= raw_size) {
        memset(encoding, 0, sizeof(*encoding));
        return 0;
    }
    if (encoding->packed_size != 0) {
        encoding->packed = (unsigned char *) calloc(encoding->packed_size, 1);
        if (encoding->packed == NULL) return -1;
    }

    i = 0;
    if (encoding->bits != 0) {
        while (length - i >= 8) {
            uint64_t word = 0;
            unsigned item, byte;
            if (position > encoding->packed_size - encoding->bits) {
                rdz_integer_encoding_free(encoding);
                return -1;
            }
            for (item = 0; item < 8; ++item) {
                word |= integer_code(values[i + item], minimum, has_na) <<
                    (item * encoding->bits);
            }
            for (byte = 0; byte < encoding->bits; ++byte) {
                encoding->packed[position++] =
                    (unsigned char) (word >> (8 * byte));
            }
            i += 8;
        }
    }
    for (; i < length; ++i) {
        uint64_t code = integer_code(values[i], minimum, has_na);
        accumulator |= code << accumulator_bits;
        accumulator_bits += encoding->bits;
        while (accumulator_bits >= 8) {
            if (position >= encoding->packed_size) {
                rdz_integer_encoding_free(encoding);
                return -1;
            }
            encoding->packed[position++] = (unsigned char) accumulator;
            accumulator >>= 8;
            accumulator_bits -= 8;
        }
    }
    if (accumulator_bits != 0) {
        if (position >= encoding->packed_size) {
            rdz_integer_encoding_free(encoding);
            return -1;
        }
        encoding->packed[position++] = (unsigned char) accumulator;
    }
    if (position != encoding->packed_size) {
        rdz_integer_encoding_free(encoding);
        return -1;
    }
    return 1;
}

void rdz_integer_encoding_free(rdz_integer_encoding_t *encoding) {
    free(encoding->packed);
    encoding->packed = NULL;
    encoding->packed_size = 0;
}

int rdz_integer_decode(const unsigned char *packed, size_t packed_size,
                           int32_t base, uint8_t bits, uint8_t flags,
                           R_xlen_t length, int *output) {
    uint64_t accumulator = 0;
    uint64_t mask = bits == 0 ? 0 : (UINT64_C(1) << bits) - 1;
    unsigned accumulator_bits = 0;
    size_t position = 0;
    int has_na = (flags & RDZ_INTEGER_HAS_NA) != 0;
    int saw_na = 0;
    int64_t maximum_value;
    R_xlen_t i = 0;

    if ((flags & ~RDZ_INTEGER_HAS_NA) != 0 ||
        bits > RDZ_INTEGER_MAX_PACKED_BITS || base == NA_INTEGER ||
        (has_na && bits == 0 && base != 0)) {
        return 0;
    }
    if (bits == 0) {
        if (packed_size != 0) return 0;
        if (has_na) {
            for (i = 0; i < length; ++i) output[i] = NA_INTEGER;
            return length != 0;
        }
        for (i = 0; i < length; ++i) output[i] = (int) base;
        return 1;
    }
    maximum_value = (int64_t) base + (int64_t) mask - (has_na != 0);
    if (maximum_value > INT_MAX) return 0;

    while (length - i >= 8) {
        uint64_t word = 0;
        unsigned item, byte;
        if (position > packed_size - bits) return 0;
        for (byte = 0; byte < bits; ++byte) {
            word |= (uint64_t) packed[position + byte] << (8 * byte);
        }
        position += bits;
        for (item = 0; item < 8; ++item) {
            uint64_t code = word & mask;
            if (has_na && code == 0) {
                output[i + item] = NA_INTEGER;
                saw_na = 1;
            } else {
                output[i + item] = (int) ((int64_t) base +
                    (int64_t) code - (has_na != 0));
            }
            word >>= bits;
        }
        i += 8;
    }
    for (; i < length; ++i) {
        uint64_t code;
        while (accumulator_bits < bits) {
            if (position >= packed_size) return 0;
            accumulator |= (uint64_t) packed[position++] << accumulator_bits;
            accumulator_bits += 8;
        }
        code = bits == 0 ? 0 : accumulator & mask;
        if (bits != 0) {
            accumulator >>= bits;
            accumulator_bits -= bits;
        }
        if (has_na && code == 0) {
            output[i] = NA_INTEGER;
            saw_na = 1;
            continue;
        }
        output[i] = (int) ((int64_t) base + (int64_t) code -
                           (has_na != 0));
    }
    return position == packed_size && accumulator == 0 &&
        (!has_na || saw_na);
}
