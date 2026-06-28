#include "logical_codec.h"

#include <R.h>

#include <stdint.h>

static int logical_code(int value, unsigned char *code) {
    if (value == 0) {
        *code = 0;
    } else if (value == 1) {
        *code = 1;
    } else if (value == NA_LOGICAL) {
        *code = 2;
    } else {
        return 0;
    }
    return 1;
}

static int logical_from_code(unsigned char code) {
    return code == 2 ? NA_LOGICAL : (int) code;
}

int fastrds_logical_packed_size(R_xlen_t length, size_t *size) {
    uint64_t quotient;
    if (length < 0 || size == NULL) return 0;
    quotient = (uint64_t) length / 4;
    if (quotient > SIZE_MAX) return 0;
    *size = (size_t) quotient + (length % 4 != 0);
    return *size >= (size_t) quotient;
}

int fastrds_logical_pack(const int *input, R_xlen_t length,
                         unsigned char *packed, size_t packed_size) {
    R_xlen_t i = 0;
    size_t position = 0;

    while (length - i >= 4) {
        unsigned char code0, code1, code2, code3;
        if (!logical_code(input[i], &code0) ||
            !logical_code(input[i + 1], &code1) ||
            !logical_code(input[i + 2], &code2) ||
            !logical_code(input[i + 3], &code3) ||
            position >= packed_size) {
            return 0;
        }
        packed[position++] = (unsigned char) (
            code0 | (code1 << 2) | (code2 << 4) | (code3 << 6)
        );
        i += 4;
    }

    if (i < length) {
        unsigned char byte = 0;
        unsigned shift = 0;
        if (position >= packed_size) return 0;
        while (i < length) {
            unsigned char code;
            if (!logical_code(input[i++], &code)) return 0;
            byte |= (unsigned char) (code << shift);
            shift += 2;
        }
        packed[position++] = byte;
    }
    return position == packed_size;
}

int fastrds_logical_unpack(const unsigned char *packed, size_t packed_size,
                           R_xlen_t length, int *output) {
    R_xlen_t i = 0;
    size_t position = 0;

    while (length - i >= 4) {
        unsigned char byte;
        if (position >= packed_size) return 0;
        byte = packed[position++];
        if ((byte & (byte >> 1) & 0x55U) != 0) return 0;
        output[i] = logical_from_code(byte & 3U);
        output[i + 1] = logical_from_code((byte >> 2) & 3U);
        output[i + 2] = logical_from_code((byte >> 4) & 3U);
        output[i + 3] = logical_from_code((byte >> 6) & 3U);
        i += 4;
    }

    if (i < length) {
        unsigned char byte, used_mask;
        unsigned remaining = (unsigned) (length - i);
        if (position >= packed_size) return 0;
        byte = packed[position++];
        used_mask = (unsigned char) ((1U << (2 * remaining)) - 1U);
        if ((byte & (unsigned char) ~used_mask) != 0 ||
            (byte & (byte >> 1) & 0x55U) != 0) {
            return 0;
        }
        while (i < length) {
            output[i++] = logical_from_code(byte & 3U);
            byte >>= 2;
        }
    }
    return position == packed_size;
}
