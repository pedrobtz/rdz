#include <R.h>
#include <Rinternals.h>

#include "integer_codec.h"
#include "lz4.h"
#include "logical_codec.h"
#include "numeric_codec.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <fcntl.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#define RDZ_CODEC_NATIVE 1
#define RDZ_CODEC_R      2
#define RDZ_MAX_DEPTH    10000
#define STRING_INDEX_BUFFER_SIZE (64 * 1024)
#define RDZ_LZ4_BLOCK_SIZE (64 * 1024)
#define STRING_LAYOUT_FLAT_RAW 0
#define STRING_LAYOUT_FLAT_LZ4 3
#define STRING_LENGTHS_DIRECT 0
#define STRING_LENGTHS_RLE 1

static const unsigned char RDZ_MAGIC_V1[8] = {
    'R', 'D', 'Z', 'F', 'I', 'L', 'E', '1'
};

static const unsigned char RDZ_MAGIC_V2[8] = {
    'R', 'D', 'Z', 'F', 'I', 'L', 'E', '2'
};

enum real_encoding {
    REAL_ENCODING_DIRECT = 0,
    REAL_ENCODING_DICTIONARY = 1,
    REAL_ENCODING_SEQUENCE = 2,
    REAL_ENCODING_DICTIONARY_LZ4 = 3,
    REAL_ENCODING_XOR_LZ4 = 4
};

enum logical_encoding {
    LOGICAL_ENCODING_DIRECT = 0,
    LOGICAL_ENCODING_PACKED = 1
};

enum integer_encoding {
    INTEGER_ENCODING_DIRECT = 0,
    INTEGER_ENCODING_PACKED = 1
};

enum node_type {
    NODE_NIL = 0,
    NODE_LOGICAL = 1,
    NODE_INTEGER = 2,
    NODE_REAL = 3,
    NODE_COMPLEX = 4,
    NODE_RAW = 5,
    NODE_STRING = 6,
    NODE_LIST = 7
};

typedef struct {
    FILE *file;
    int failed;
} writer_t;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos;
#ifndef _WIN32
    int fd;
#endif
} reader_t;

typedef struct {
    SEXP *items;
    size_t size;
    size_t capacity;
} sexp_stack_t;

typedef struct {
    SEXP *keys;
    uint64_t *values;
    size_t capacity;
    size_t size;
    SEXP *unique;
    size_t unique_capacity;
} string_dict_t;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
    unsigned char *input;
    char *compressed;
    size_t input_size;
    size_t block_count;
    int compressed_capacity;
    int rejected;
} lz4_blocks_t;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} byte_buffer_t;

static unsigned char host_endian(void) {
    const uint16_t x = 1;
    return *((const unsigned char *) &x) == 1 ? 1 : 2;
}

static void writer_write(writer_t *writer, const void *data, size_t size) {
    if (writer->failed || size == 0) return;
    if (fwrite(data, 1, size, writer->file) != size) writer->failed = 1;
}

static void writer_u8(writer_t *writer, uint8_t value) {
    writer_write(writer, &value, sizeof(value));
}

static void writer_u16(writer_t *writer, uint16_t value) {
    writer_write(writer, &value, sizeof(value));
}

static void writer_u32(writer_t *writer, uint32_t value) {
    writer_write(writer, &value, sizeof(value));
}

static void writer_u64(writer_t *writer, uint64_t value) {
    writer_write(writer, &value, sizeof(value));
}

static void reader_need(reader_t *reader, size_t size) {
    if (size > reader->size - reader->pos) {
        Rf_error("invalid or truncated rdz file");
    }
}

static void reader_read(reader_t *reader, void *output, size_t size) {
    reader_need(reader, size);
    if (size != 0) memcpy(output, reader->data + reader->pos, size);
    reader->pos += size;
}

static void reader_payload(reader_t *reader, void *output, size_t size) {
    reader_need(reader, size);
#ifndef _WIN32
    if (size >= 64 * 1024) {
        size_t copied = 0;
        while (copied < size) {
            ssize_t count = pread(reader->fd, (unsigned char *) output + copied,
                                  size - copied, (off_t) (reader->pos + copied));
            if (count <= 0) Rf_error("failed while reading rdz payload");
            copied += (size_t) count;
        }
        reader->pos += size;
        return;
    }
#endif
    reader_read(reader, output, size);
}

static uint8_t reader_u8(reader_t *reader) {
    uint8_t value;
    reader_read(reader, &value, sizeof(value));
    return value;
}

static uint16_t reader_u16(reader_t *reader) {
    uint16_t value;
    reader_read(reader, &value, sizeof(value));
    return value;
}

static uint32_t reader_u32(reader_t *reader) {
    uint32_t value;
    reader_read(reader, &value, sizeof(value));
    return value;
}

static uint64_t reader_u64(reader_t *reader) {
    uint64_t value;
    reader_read(reader, &value, sizeof(value));
    return value;
}

static void reader_lz4_blocks(reader_t *reader, unsigned char *output,
                              size_t output_size, const char *error_message) {
    size_t output_pos = 0;

    while (output_pos < output_size) {
        size_t remaining = output_size - output_pos;
        size_t block_size = remaining < RDZ_LZ4_BLOCK_SIZE ?
            remaining : RDZ_LZ4_BLOCK_SIZE;
        uint32_t stored_size = reader_u32(reader);
        if (stored_size == 0 || (size_t) stored_size > block_size) {
            Rf_error("%s", error_message);
        }
        reader_need(reader, (size_t) stored_size);
        if ((size_t) stored_size == block_size) {
            memcpy(output + output_pos, reader->data + reader->pos, block_size);
        } else {
            int decoded = LZ4_decompress_safe(
                (const char *) reader->data + reader->pos,
                (char *) output + output_pos,
                (int) stored_size, (int) block_size
            );
            if (decoded != (int) block_size) Rf_error("%s", error_message);
        }
        reader->pos += (size_t) stored_size;
        output_pos += block_size;
    }
}

static void reader_real_xor_lz4(reader_t *reader, double *output,
                                R_xlen_t length) {
    unsigned char *transformed;
    uint64_t previous = 0;
    R_xlen_t position = 0;
    const char *error_message =
        "invalid LZ4 XOR numeric block in rdz file";

    if (length <= 0) Rf_error("%s", error_message);
    transformed = (unsigned char *) R_alloc(RDZ_LZ4_BLOCK_SIZE, 1);
    while (position < length) {
        R_xlen_t remaining = length - position;
        size_t count = remaining < RDZ_REAL_XOR_BLOCK_VALUES ?
            (size_t) remaining : RDZ_REAL_XOR_BLOCK_VALUES;
        size_t block_size = count * sizeof(uint64_t);
        uint32_t stored_size = reader_u32(reader);
        if (stored_size == 0 || (size_t) stored_size > block_size) {
            Rf_error("%s", error_message);
        }
        reader_need(reader, (size_t) stored_size);
        if ((size_t) stored_size == block_size) {
            memcpy(transformed, reader->data + reader->pos, block_size);
        } else {
            int decoded = LZ4_decompress_safe(
                (const char *) reader->data + reader->pos,
                (char *) transformed,
                (int) stored_size, (int) block_size
            );
            if (decoded != (int) block_size) Rf_error("%s", error_message);
        }
        rdz_real_xor_decode_block(
            transformed, count, &previous, output + position);
        reader->pos += (size_t) stored_size;
        position += (R_xlen_t) count;
    }
}

static int buffer_varint(const unsigned char **cursor,
                         const unsigned char *end, uint64_t *value) {
    uint64_t result = 0;
    unsigned i;

    for (i = 0; i < 10; ++i) {
        unsigned char byte;
        if (*cursor == end) return 0;
        byte = *(*cursor)++;
        if (i == 9 && (byte & 0xfe) != 0) return 0;
        result |= (uint64_t) (byte & 0x7f) << (i * 7);
        if ((byte & 0x80) == 0) {
            *value = result;
            return 1;
        }
    }
    return 0;
}

static int stack_contains(const sexp_stack_t *stack, SEXP object) {
    size_t i;
    for (i = 0; i < stack->size; ++i) {
        if (stack->items[i] == object) return 1;
    }
    return 0;
}

static int stack_push(sexp_stack_t *stack, SEXP object) {
    if (stack->size == stack->capacity) {
        size_t capacity = stack->capacity == 0 ? 32 : stack->capacity * 2;
        SEXP *items = (SEXP *) realloc(stack->items, capacity * sizeof(SEXP));
        if (items == NULL) return 0;
        stack->items = items;
        stack->capacity = capacity;
    }
    stack->items[stack->size++] = object;
    return 1;
}

static int native_supported(SEXP object, sexp_stack_t *stack, int depth) {
    SEXP attribute;
    R_xlen_t i;
    int type = TYPEOF(object);

    if (depth > RDZ_MAX_DEPTH || Rf_isS4(object) ||
        (object != R_NilValue && ALTREP(object) &&
         DATAPTR_OR_NULL(object) == NULL)) return 0;
    switch (type) {
    case NILSXP:
    case LGLSXP:
    case INTSXP:
    case REALSXP:
    case CPLXSXP:
    case RAWSXP:
    case STRSXP:
    case VECSXP:
        break;
    default:
        return 0;
    }

    if (object != R_NilValue) {
        if (stack_contains(stack, object) || !stack_push(stack, object)) return 0;
    }

    for (attribute = ATTRIB(object); attribute != R_NilValue;
         attribute = CDR(attribute)) {
        if (TYPEOF(TAG(attribute)) != SYMSXP ||
            !native_supported(CAR(attribute), stack, depth + 1)) {
            if (object != R_NilValue) --stack->size;
            return 0;
        }
    }

    if (type == VECSXP) {
        for (i = 0; i < XLENGTH(object); ++i) {
            if (!native_supported(VECTOR_ELT(object, i), stack, depth + 1)) {
                --stack->size;
                return 0;
            }
        }
    }

    if (object != R_NilValue) --stack->size;
    return 1;
}

static uint64_t pointer_hash(SEXP value) {
    uint64_t x = (uint64_t) (uintptr_t) value;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

static int dict_rehash(string_dict_t *dict, size_t capacity) {
    SEXP *keys = (SEXP *) calloc(capacity, sizeof(SEXP));
    uint64_t *values = (uint64_t *) calloc(capacity, sizeof(uint64_t));
    size_t i;
    if (keys == NULL || values == NULL) {
        free(keys);
        free(values);
        return 0;
    }
    for (i = 0; i < dict->capacity; ++i) {
        if (dict->keys[i] != NULL) {
            size_t slot = (size_t) pointer_hash(dict->keys[i]) & (capacity - 1);
            while (keys[slot] != NULL) slot = (slot + 1) & (capacity - 1);
            keys[slot] = dict->keys[i];
            values[slot] = dict->values[i];
        }
    }
    free(dict->keys);
    free(dict->values);
    dict->keys = keys;
    dict->values = values;
    dict->capacity = capacity;
    return 1;
}

static uint64_t dict_index(string_dict_t *dict, SEXP value, int add) {
    size_t slot;
    if (value == NA_STRING) return 0;
    if (dict->capacity == 0 && !dict_rehash(dict, 16)) return UINT64_MAX;
    if (add && (dict->size + 1) * 10 >= dict->capacity * 7) {
        if (!dict_rehash(dict, dict->capacity * 2)) return UINT64_MAX;
    }
    slot = (size_t) pointer_hash(value) & (dict->capacity - 1);
    while (dict->keys[slot] != NULL && dict->keys[slot] != value) {
        slot = (slot + 1) & (dict->capacity - 1);
    }
    if (dict->keys[slot] == value) return dict->values[slot];
    if (!add) return UINT64_MAX;

    if (dict->size == dict->unique_capacity) {
        size_t capacity = dict->unique_capacity == 0 ? 16 : dict->unique_capacity * 2;
        SEXP *unique = (SEXP *) realloc(dict->unique, capacity * sizeof(SEXP));
        if (unique == NULL) return UINT64_MAX;
        dict->unique = unique;
        dict->unique_capacity = capacity;
    }
    dict->keys[slot] = value;
    dict->values[slot] = (uint64_t) dict->size + 1;
    dict->unique[dict->size++] = value;
    return (uint64_t) dict->size;
}

static void dict_free(string_dict_t *dict) {
    free(dict->keys);
    free(dict->values);
    free(dict->unique);
}

static void byte_buffer_free(byte_buffer_t *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int byte_buffer_reserve(byte_buffer_t *buffer, size_t additional) {
    size_t required, capacity;
    unsigned char *data;

    if (additional > SIZE_MAX - buffer->size) return 0;
    required = buffer->size + additional;
    if (required <= buffer->capacity) return 1;
    capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    data = (unsigned char *) realloc(buffer->data, capacity);
    if (data == NULL) return 0;
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int byte_buffer_varint(byte_buffer_t *buffer, uint64_t value) {
    unsigned char encoded[10];
    size_t size = 0;

    do {
        encoded[size] = (unsigned char) (value & UINT64_C(0x7f));
        value >>= 7;
        if (value != 0) encoded[size] |= 0x80;
        ++size;
    } while (value != 0);
    if (!byte_buffer_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, encoded, size);
    buffer->size += size;
    return 1;
}

static int string_lengths_append_run(byte_buffer_t *buffer, uint64_t count,
                                     int32_t length) {
    uint64_t code = length < 0 ? 0 : (uint64_t) (uint32_t) length + 1;
    return byte_buffer_varint(buffer, count) &&
           byte_buffer_varint(buffer, code);
}

static void lz4_blocks_free(lz4_blocks_t *blocks) {
    free(blocks->data);
    free(blocks->input);
    free(blocks->compressed);
    memset(blocks, 0, sizeof(*blocks));
}

static int lz4_blocks_reserve(lz4_blocks_t *blocks, size_t additional) {
    size_t required, capacity;
    unsigned char *data;

    if (additional > SIZE_MAX - blocks->size) return 0;
    required = blocks->size + additional;
    if (required <= blocks->capacity) return 1;
    capacity = blocks->capacity == 0 ? RDZ_LZ4_BLOCK_SIZE : blocks->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    data = (unsigned char *) realloc(blocks->data, capacity);
    if (data == NULL) return 0;
    blocks->data = data;
    blocks->capacity = capacity;
    return 1;
}

static int lz4_blocks_flush(lz4_blocks_t *blocks) {
    int compressed_size;
    size_t stored_size, record_size;
    uint32_t stored_size32;
    const unsigned char *stored;

    if (blocks->input_size == 0 || blocks->rejected) return 1;
    compressed_size = LZ4_compress_default(
        (const char *) blocks->input, blocks->compressed,
        (int) blocks->input_size, blocks->compressed_capacity
    );
    if (compressed_size <= 0) {
        blocks->rejected = 1;
        return 0;
    }

    /* One representative incompressible block is enough to retain the raw
       layout. This bounds memory and avoids unnecessary compression work. */
    if (blocks->block_count == 0 &&
        (size_t) compressed_size + sizeof(uint32_t) >= blocks->input_size) {
        blocks->rejected = 1;
        blocks->input_size = 0;
        return 1;
    }

    if ((size_t) compressed_size < blocks->input_size) {
        stored_size = (size_t) compressed_size;
        stored = (const unsigned char *) blocks->compressed;
    } else {
        stored_size = blocks->input_size;
        stored = blocks->input;
    }
    record_size = sizeof(uint32_t) + stored_size;
    if (!lz4_blocks_reserve(blocks, record_size)) {
        blocks->rejected = 1;
        return 0;
    }
    stored_size32 = (uint32_t) stored_size;
    memcpy(blocks->data + blocks->size, &stored_size32, sizeof(stored_size32));
    memcpy(blocks->data + blocks->size + sizeof(stored_size32),
           stored, stored_size);
    blocks->size += record_size;
    blocks->input_size = 0;
    ++blocks->block_count;
    return 1;
}

static int lz4_blocks_append(lz4_blocks_t *blocks,
                             const unsigned char *data, size_t size) {
    while (size != 0 && !blocks->rejected) {
        size_t available = RDZ_LZ4_BLOCK_SIZE - blocks->input_size;
        size_t take = size < available ? size : available;
        memcpy(blocks->input + blocks->input_size, data, take);
        blocks->input_size += take;
        data += take;
        size -= take;
        if (blocks->input_size == RDZ_LZ4_BLOCK_SIZE &&
            !lz4_blocks_flush(blocks)) {
            return 0;
        }
    }
    return !blocks->rejected;
}

static int lz4_blocks_build_strings(SEXP object, uint64_t payload_size,
                                    lz4_blocks_t *blocks) {
    R_xlen_t i, length = XLENGTH(object);
    int built;

    memset(blocks, 0, sizeof(*blocks));
    if (payload_size == 0 || payload_size > SIZE_MAX) return 0;
    blocks->compressed_capacity = LZ4_compressBound(RDZ_LZ4_BLOCK_SIZE);
    if (blocks->compressed_capacity <= 0) return 0;
    blocks->input = (unsigned char *) malloc(RDZ_LZ4_BLOCK_SIZE);
    blocks->compressed = (char *) malloc((size_t) blocks->compressed_capacity);
    if (blocks->input == NULL || blocks->compressed == NULL) {
        lz4_blocks_free(blocks);
        return 0;
    }

    for (i = 0; i < length && !blocks->rejected; ++i) {
        SEXP string = STRING_ELT(object, i);
        if (string != NA_STRING &&
            !lz4_blocks_append(
                blocks, (const unsigned char *) CHAR(string),
                (size_t) LENGTH(string))) {
            break;
        }
    }
    if (!blocks->rejected) (void) lz4_blocks_flush(blocks);
    built = !blocks->rejected;
    free(blocks->input);
    free(blocks->compressed);
    blocks->input = NULL;
    blocks->compressed = NULL;
    if (!built) {
        free(blocks->data);
        blocks->data = NULL;
        blocks->size = 0;
        blocks->capacity = 0;
    }
    return built;
}

static int lz4_blocks_build_buffer(const unsigned char *input,
                                   size_t input_size, lz4_blocks_t *blocks) {
    size_t input_pos = 0;

    memset(blocks, 0, sizeof(*blocks));
    if (input_size == 0) return 0;
    blocks->compressed_capacity = LZ4_compressBound(RDZ_LZ4_BLOCK_SIZE);
    if (blocks->compressed_capacity <= 0) return 0;
    blocks->compressed = (char *) malloc((size_t) blocks->compressed_capacity);
    if (blocks->compressed == NULL) {
        lz4_blocks_free(blocks);
        return 0;
    }

    while (input_pos < input_size) {
        size_t remaining = input_size - input_pos;
        size_t block_size = remaining < RDZ_LZ4_BLOCK_SIZE ?
            remaining : RDZ_LZ4_BLOCK_SIZE;
        int compressed_size = LZ4_compress_default(
            (const char *) input + input_pos, blocks->compressed,
            (int) block_size, blocks->compressed_capacity
        );
        const unsigned char *stored;
        size_t stored_size, record_size;
        uint32_t stored_size32;

        if (compressed_size <= 0 ||
            (blocks->block_count == 0 &&
             (size_t) compressed_size + sizeof(uint32_t) >= block_size)) {
            blocks->rejected = 1;
            break;
        }
        if ((size_t) compressed_size < block_size) {
            stored = (const unsigned char *) blocks->compressed;
            stored_size = (size_t) compressed_size;
        } else {
            stored = input + input_pos;
            stored_size = block_size;
        }
        record_size = sizeof(uint32_t) + stored_size;
        if (!lz4_blocks_reserve(blocks, record_size)) {
            blocks->rejected = 1;
            break;
        }
        stored_size32 = (uint32_t) stored_size;
        memcpy(blocks->data + blocks->size, &stored_size32,
               sizeof(stored_size32));
        memcpy(blocks->data + blocks->size + sizeof(stored_size32),
               stored, stored_size);
        blocks->size += record_size;
        ++blocks->block_count;
        input_pos += block_size;
    }

    free(blocks->compressed);
    blocks->compressed = NULL;
    if (blocks->rejected || blocks->size >= input_size) {
        free(blocks->data);
        blocks->data = NULL;
        blocks->size = 0;
        blocks->capacity = 0;
        return 0;
    }
    return 1;
}

static int lz4_blocks_build_real_xor(SEXP object, lz4_blocks_t *blocks) {
    const double *values = REAL_RO(object);
    R_xlen_t length = XLENGTH(object), position = 0;
    uint64_t previous = 0;
    size_t raw_size;
    int built;

    memset(blocks, 0, sizeof(*blocks));
    if (length <= 0 || (uint64_t) length > SIZE_MAX / sizeof(double)) return 0;
    raw_size = (size_t) length * sizeof(double);
    blocks->compressed_capacity = LZ4_compressBound(RDZ_LZ4_BLOCK_SIZE);
    if (blocks->compressed_capacity <= 0) return 0;
    blocks->input = (unsigned char *) malloc(RDZ_LZ4_BLOCK_SIZE);
    blocks->compressed = (char *) malloc((size_t) blocks->compressed_capacity);
    if (blocks->input == NULL || blocks->compressed == NULL) {
        lz4_blocks_free(blocks);
        return 0;
    }

    while (position < length && !blocks->rejected) {
        R_xlen_t remaining = length - position;
        size_t count = remaining < RDZ_REAL_XOR_BLOCK_VALUES ?
            (size_t) remaining : RDZ_REAL_XOR_BLOCK_VALUES;
        rdz_real_xor_encode_block(
            values + position, count, &previous, blocks->input);
        blocks->input_size = count * sizeof(uint64_t);
        if (!lz4_blocks_flush(blocks)) break;
        if (blocks->block_count == 1 &&
            blocks->size > count * sizeof(uint64_t) * 7 / 8) {
            blocks->rejected = 1;
            break;
        }
        position += (R_xlen_t) count;
    }
    built = !blocks->rejected && position == length &&
        blocks->size <= raw_size - raw_size / 8;
    free(blocks->input);
    free(blocks->compressed);
    blocks->input = NULL;
    blocks->compressed = NULL;
    if (!built) {
        free(blocks->data);
        blocks->data = NULL;
        blocks->size = 0;
        blocks->capacity = 0;
    }
    return built;
}

static void encode_string_indexes(writer_t *writer, SEXP object,
                                  string_dict_t *dict, uint8_t width) {
    R_xlen_t i, length = XLENGTH(object);
    unsigned char *buffer;
    size_t used = 0;

    if (length == 0) return;
    if (width != 1 && width != 2 && width != 4) {
        writer->failed = 1;
        return;
    }
    buffer = (unsigned char *) malloc(STRING_INDEX_BUFFER_SIZE);
    if (buffer == NULL) {
        writer->failed = 1;
        return;
    }
    for (i = 0; i < length && !writer->failed; ++i) {
        uint64_t index = dict_index(dict, STRING_ELT(object, i), 0);
        if (index == UINT64_MAX) {
            writer->failed = 1;
            break;
        }
        if (width == 1) {
            buffer[used] = (unsigned char) index;
        } else if (width == 2) {
            uint16_t value = (uint16_t) index;
            memcpy(buffer + used, &value, sizeof(value));
        } else {
            uint32_t value = (uint32_t) index;
            memcpy(buffer + used, &value, sizeof(value));
        }
        used += width;
        if (used == STRING_INDEX_BUFFER_SIZE) {
            writer_write(writer, buffer, used);
            used = 0;
        }
    }
    writer_write(writer, buffer, used);
    free(buffer);
}

static cetype_t normalized_encoding(SEXP string) {
    cetype_t encoding = Rf_getCharCE(string);
    if (encoding != CE_NATIVE && encoding != CE_UTF8 &&
        encoding != CE_LATIN1 && encoding != CE_BYTES) {
        return CE_NATIVE;
    }
    return encoding;
}

static int strings_should_use_flat(SEXP object) {
    R_xlen_t length = XLENGTH(object);
    R_xlen_t sample_size = length < 1024 ? length : 1024;
    SEXP seen[1024];
    R_xlen_t i, j, distinct = 0, non_missing = 0;

    if (sample_size == 0) return 0;
    for (i = 0; i < sample_size; ++i) {
        R_xlen_t at = sample_size == length ? i :
            (R_xlen_t) (((uint64_t) i * (uint64_t) length) /
                        (uint64_t) sample_size);
        SEXP string = STRING_ELT(object, at);
        int found = 0;
        if (string == NA_STRING) continue;
        ++non_missing;
        for (j = 0; j < distinct; ++j) {
            if (seen[j] == string) {
                found = 1;
                break;
            }
        }
        if (!found) seen[distinct++] = string;
    }
    return non_missing != 0 && distinct * 4 >= non_missing * 3;
}

static void write_string_payload_raw(writer_t *writer, SEXP object) {
    R_xlen_t i, length = XLENGTH(object);
    unsigned char *chunk;
    size_t chunk_size = 1024 * 1024, chunk_used = 0;

    chunk = (unsigned char *) malloc(chunk_size);
    if (chunk == NULL) {
        writer->failed = 1;
        return;
    }
    for (i = 0; i < length; ++i) {
        SEXP string = STRING_ELT(object, i);
        size_t remaining, offset = 0;
        if (string == NA_STRING) continue;
        remaining = (size_t) LENGTH(string);
        while (remaining != 0) {
            size_t available = chunk_size - chunk_used;
            size_t take = remaining < available ? remaining : available;
            memcpy(chunk + chunk_used, CHAR(string) + offset, take);
            chunk_used += take;
            offset += take;
            remaining -= take;
            if (chunk_used == chunk_size) {
                writer_write(writer, chunk, chunk_used);
                chunk_used = 0;
            }
        }
    }
    writer_write(writer, chunk, chunk_used);
    free(chunk);
}

static void encode_strings_flat(writer_t *writer, SEXP object,
                                int format_version) {
    R_xlen_t i, length = XLENGTH(object);
    uint64_t payload_size = 0;
    uint8_t common_encoding = UINT8_MAX;
    int mixed_encoding = 0;
    int lz4_candidate = 0;
    int use_lz4 = 0;
    int rle_failed = 0;
    int32_t rle_value = 0;
    uint64_t rle_count = 0;
    size_t raw_lengths_size, metadata_size;
    unsigned char *metadata, *lengths, *encodings = NULL;
    lz4_blocks_t blocks;
    byte_buffer_t rle;

    memset(&blocks, 0, sizeof(blocks));
    memset(&rle, 0, sizeof(rle));

    for (i = 0; i < length; ++i) {
        SEXP string = STRING_ELT(object, i);
        if (string != NA_STRING) {
            uint64_t string_length = (uint64_t) LENGTH(string);
            uint8_t encoding = (uint8_t) normalized_encoding(string);
            if (UINT64_MAX - payload_size < string_length) {
                writer->failed = 1;
                return;
            }
            payload_size += string_length;
            if (common_encoding == UINT8_MAX) common_encoding = encoding;
            else if (common_encoding != encoding) mixed_encoding = 1;
        }
    }
    if (common_encoding == UINT8_MAX) common_encoding = CE_NATIVE;
    if ((uint64_t) length > SIZE_MAX / sizeof(int32_t)) {
        writer->failed = 1;
        return;
    }
    raw_lengths_size = (size_t) length * sizeof(int32_t);
    metadata_size = raw_lengths_size;
    if (mixed_encoding) {
        if ((uint64_t) length > SIZE_MAX - metadata_size) {
            writer->failed = 1;
            return;
        }
        metadata_size += (size_t) length;
    }
    metadata = (unsigned char *) malloc(metadata_size == 0 ? 1 : metadata_size);
    if (metadata == NULL) {
        writer->failed = 1;
        return;
    }
    lengths = metadata;
    if (mixed_encoding) encodings = metadata + (size_t) length * sizeof(int32_t);
    for (i = 0; i < length; ++i) {
        SEXP string = STRING_ELT(object, i);
        int32_t string_length = string == NA_STRING ? -1 : (int32_t) LENGTH(string);
        memcpy(lengths + (size_t) i * sizeof(int32_t),
               &string_length, sizeof(string_length));
        if (format_version >= 2 && !rle_failed) {
            if (rle_count == 0) {
                rle_value = string_length;
                rle_count = 1;
            } else if (string_length == rle_value) {
                ++rle_count;
            } else {
                if (!string_lengths_append_run(&rle, rle_count, rle_value)) {
                    rle_failed = 1;
                }
                rle_value = string_length;
                rle_count = 1;
            }
        }
        if (mixed_encoding) {
            encodings[i] = string == NA_STRING ? CE_NATIVE :
                (uint8_t) normalized_encoding(string);
        }
    }
    if (format_version >= 2 && !rle_failed && rle_count != 0 &&
        !string_lengths_append_run(&rle, rle_count, rle_value)) {
        rle_failed = 1;
    }

    if (format_version >= 2) {
        size_t length_data_size = raw_lengths_size;
        uint64_t raw_size, compressed_size;

        lz4_candidate = lz4_blocks_build_strings(object, payload_size, &blocks);
        if (!rle_failed && rle.size < raw_lengths_size) {
            length_data_size = rle.size;
        }
        raw_size = payload_size;
        compressed_size = (uint64_t) blocks.size;
        if (lz4_candidate &&
            raw_lengths_size <= UINT64_MAX - raw_size &&
            (mixed_encoding ? (uint64_t) length : 0) <=
                UINT64_MAX - raw_size - raw_lengths_size &&
            compressed_size <= UINT64_MAX - 9 &&
            length_data_size <= UINT64_MAX - compressed_size - 9 &&
            (mixed_encoding ? (uint64_t) length : 0) <=
                UINT64_MAX - compressed_size - 9 - length_data_size) {
            raw_size += (uint64_t) raw_lengths_size +
                (mixed_encoding ? (uint64_t) length : 0);
            compressed_size += 9 + (uint64_t) length_data_size +
                (mixed_encoding ? (uint64_t) length : 0);
            use_lz4 = compressed_size < raw_size;
        }
    }
    writer_u8(writer, use_lz4 ? STRING_LAYOUT_FLAT_LZ4 :
                               STRING_LAYOUT_FLAT_RAW);
    writer_u8(writer, mixed_encoding ? UINT8_MAX : common_encoding);
    writer_u64(writer, payload_size);

    if (use_lz4) {
        uint8_t length_encoding =
            !rle_failed && rle.size < raw_lengths_size ?
            STRING_LENGTHS_RLE : STRING_LENGTHS_DIRECT;
        const unsigned char *length_data =
            length_encoding == STRING_LENGTHS_RLE ? rle.data : lengths;
        size_t length_data_size =
            length_encoding == STRING_LENGTHS_RLE ? rle.size : raw_lengths_size;
        writer_u8(writer, length_encoding);
        writer_u64(writer, (uint64_t) length_data_size);
        writer_write(writer, length_data, length_data_size);
        if (mixed_encoding) writer_write(writer, encodings, (size_t) length);
        writer_write(writer, blocks.data, blocks.size);
    } else {
        writer_write(writer, metadata, metadata_size);
        write_string_payload_raw(writer, object);
    }
    free(metadata);
    byte_buffer_free(&rle);
    lz4_blocks_free(&blocks);
}

static void encode_node(writer_t *writer, SEXP object, int depth,
                        int format_version);

static void encode_attributes(writer_t *writer, SEXP object, int depth,
                              int format_version) {
    SEXP attribute;
    uint32_t count = 0;
    for (attribute = ATTRIB(object); attribute != R_NilValue;
         attribute = CDR(attribute)) {
        if (count == UINT32_MAX) {
            writer->failed = 1;
            return;
        }
        ++count;
    }
    writer_u32(writer, count);
    for (attribute = ATTRIB(object); attribute != R_NilValue;
         attribute = CDR(attribute)) {
        SEXP name = PRINTNAME(TAG(attribute));
        uint32_t length = (uint32_t) LENGTH(name);
        writer_u32(writer, length);
        writer_write(writer, CHAR(name), length);
        encode_node(writer, CAR(attribute), depth + 1, format_version);
    }
}

static void encode_strings(writer_t *writer, SEXP object,
                           int format_version) {
    string_dict_t dict;
    R_xlen_t i, length = XLENGTH(object);
    uint8_t width;
    memset(&dict, 0, sizeof(dict));

    if (strings_should_use_flat(object)) {
        encode_strings_flat(writer, object, format_version);
        return;
    }

    for (i = 0; i < length; ++i) {
        if (dict_index(&dict, STRING_ELT(object, i), 1) == UINT64_MAX) {
            writer->failed = 1;
            dict_free(&dict);
            return;
        }
    }

    width = dict.size <= UINT8_MAX ? 1 : (dict.size <= UINT16_MAX ? 2 : 4);
    writer_u8(writer, width);
    writer_u64(writer, (uint64_t) dict.size);
    for (i = 0; i < (R_xlen_t) dict.size; ++i) {
        SEXP string = dict.unique[i];
        cetype_t encoding = normalized_encoding(string);
        uint64_t string_length = (uint64_t) LENGTH(string);
        writer_u8(writer, (uint8_t) encoding);
        writer_u64(writer, string_length);
        writer_write(writer, CHAR(string), (size_t) string_length);
    }

    encode_string_indexes(writer, object, &dict, width);
    dict_free(&dict);
}

static void encode_node(writer_t *writer, SEXP object, int depth,
                        int format_version) {
    R_xlen_t i, length;
    uint8_t type;
    size_t element_size = 0;

    if (writer->failed || depth > RDZ_MAX_DEPTH) {
        writer->failed = 1;
        return;
    }
    switch (TYPEOF(object)) {
    case NILSXP: type = NODE_NIL; break;
    case LGLSXP: type = NODE_LOGICAL; element_size = sizeof(int); break;
    case INTSXP: type = NODE_INTEGER; element_size = sizeof(int); break;
    case REALSXP: type = NODE_REAL; element_size = sizeof(double); break;
    case CPLXSXP: type = NODE_COMPLEX; element_size = sizeof(Rcomplex); break;
    case RAWSXP: type = NODE_RAW; element_size = sizeof(Rbyte); break;
    case STRSXP: type = NODE_STRING; break;
    case VECSXP: type = NODE_LIST; break;
    default:
        writer->failed = 1;
        return;
    }

    length = object == R_NilValue ? 0 : XLENGTH(object);
    writer_u8(writer, type);
    writer_u8(writer, object == R_NilValue ? 0 : (uint8_t) Rf_isObject(object));
    writer_u64(writer, (uint64_t) length);
    encode_attributes(writer, object, depth, format_version);

    if (element_size != 0) {
        if (type == NODE_LOGICAL && format_version >= 2) {
            size_t packed_size;
            unsigned char *packed;
            if (!rdz_logical_packed_size(length, &packed_size)) {
                writer->failed = 1;
                return;
            }
            packed = (unsigned char *) malloc(packed_size == 0 ? 1 : packed_size);
            if (packed == NULL) {
                writer->failed = 1;
                return;
            }
            if (rdz_logical_pack(LOGICAL(object), length, packed,
                                     packed_size)) {
                writer_u8(writer, LOGICAL_ENCODING_PACKED);
                writer_write(writer, packed, packed_size);
                free(packed);
                return;
            }
            free(packed);
            writer_u8(writer, LOGICAL_ENCODING_DIRECT);
        }
        if (type == NODE_INTEGER && format_version >= 2) {
            rdz_integer_encoding_t encoding;
            int encoded = rdz_integer_encoding_build(object, &encoding);
            if (encoded < 0) {
                writer->failed = 1;
                return;
            }
            if (encoded) {
                writer_u8(writer, INTEGER_ENCODING_PACKED);
                writer_u32(writer, (uint32_t) encoding.base);
                writer_u8(writer, encoding.bits);
                writer_u8(writer, encoding.flags);
                writer_write(writer, encoding.packed, encoding.packed_size);
                rdz_integer_encoding_free(&encoding);
                return;
            }
            writer_u8(writer, INTEGER_ENCODING_DIRECT);
        }
        if (type == NODE_REAL && format_version >= 2) {
            rdz_real_dictionary_t dictionary;
            rdz_real_sequence_t sequence;
            int encoded = rdz_real_dictionary_build(object, &dictionary);
            if (encoded < 0) {
                writer->failed = 1;
                return;
            }
            if (encoded) {
                lz4_blocks_t blocks;
                int use_lz4 = lz4_blocks_build_buffer(
                    dictionary.packed, dictionary.packed_size, &blocks);
                writer_u8(writer, use_lz4 ? REAL_ENCODING_DICTIONARY_LZ4 :
                                           REAL_ENCODING_DICTIONARY);
                writer_u8(writer, dictionary.bits);
                writer_u16(writer, dictionary.count);
                writer_write(writer, dictionary.values,
                             (size_t) dictionary.count * sizeof(uint64_t));
                if (use_lz4) {
                    writer_write(writer, blocks.data, blocks.size);
                } else {
                    writer_write(writer, dictionary.packed,
                                 dictionary.packed_size);
                }
                lz4_blocks_free(&blocks);
                rdz_real_dictionary_free(&dictionary);
                return;
            }
            if (rdz_real_sequence_build(object, &sequence)) {
                writer_u8(writer, REAL_ENCODING_SEQUENCE);
                writer_u64(writer, sequence.base);
                writer_u64(writer, sequence.delta);
                return;
            }
            {
                lz4_blocks_t blocks;
                if (lz4_blocks_build_real_xor(object, &blocks)) {
                    writer_u8(writer, REAL_ENCODING_XOR_LZ4);
                    writer_write(writer, blocks.data, blocks.size);
                    lz4_blocks_free(&blocks);
                    return;
                }
                lz4_blocks_free(&blocks);
            }
            writer_u8(writer, REAL_ENCODING_DIRECT);
        }
        if ((uint64_t) length > SIZE_MAX / element_size) {
            writer->failed = 1;
            return;
        }
        writer_write(writer, DATAPTR_RO(object), (size_t) length * element_size);
    } else if (type == NODE_STRING) {
        encode_strings(writer, object, format_version);
    } else if (type == NODE_LIST) {
        for (i = 0; i < length; ++i) {
            encode_node(writer, VECTOR_ELT(object, i), depth + 1,
                        format_version);
        }
    }
}

static SEXP decode_node(reader_t *reader, int depth, int format_version);

static void decode_attributes(reader_t *reader, SEXP object, uint32_t count,
                              int depth, int format_version) {
    uint32_t i;
    for (i = 0; i < count; ++i) {
        uint32_t length = reader_u32(reader);
        char *name;
        SEXP tag, value;
        reader_need(reader, length);
        name = (char *) malloc((size_t) length + 1);
        if (name == NULL) Rf_error("unable to allocate attribute name");
        memcpy(name, reader->data + reader->pos, length);
        name[length] = '\0';
        reader->pos += length;
        tag = Rf_install(name);
        free(name);
        PROTECT(tag);
        value = PROTECT(decode_node(reader, depth + 1, format_version));
        Rf_setAttrib(object, tag, value);
        UNPROTECT(2);
    }
}

static cetype_t decode_encoding(uint8_t value) {
    if (value == CE_NATIVE || value == CE_UTF8 ||
        value == CE_LATIN1 || value == CE_BYTES) {
        return (cetype_t) value;
    }
    Rf_error("invalid character encoding in rdz file");
    return CE_NATIVE;
}

static void decode_strings(reader_t *reader, SEXP object, R_xlen_t length,
                           int format_version) {
    uint8_t layout = reader_u8(reader);
    uint64_t dict_size64;
    R_xlen_t dict_size, i;
    SEXP dictionary;

    if (layout == STRING_LAYOUT_FLAT_RAW ||
        (layout == STRING_LAYOUT_FLAT_LZ4 && format_version >= 2)) {
        uint8_t common_encoding = reader_u8(reader);
        uint64_t payload_size64 = reader_u64(reader);
        size_t lengths_size;
        const unsigned char *lengths, *encodings = NULL, *payload, *cursor, *end;
        int32_t *decoded_lengths = NULL;
        SEXP payload_buffer = R_NilValue;
        int payload_protected = 0;

        if ((uint64_t) length > SIZE_MAX / sizeof(int32_t) ||
            payload_size64 > SIZE_MAX) {
            Rf_error("string vector is too large");
        }
        lengths_size = (size_t) length * sizeof(int32_t);
        if (layout == STRING_LAYOUT_FLAT_LZ4) {
            uint8_t length_encoding = reader_u8(reader);
            uint64_t encoded_size64 = reader_u64(reader);
            const unsigned char *metadata_cursor, *metadata_end;

            if (encoded_size64 > SIZE_MAX) {
                Rf_error("string length metadata is too large");
            }
            reader_need(reader, (size_t) encoded_size64);
            metadata_cursor = reader->data + reader->pos;
            metadata_end = metadata_cursor + (size_t) encoded_size64;
            if (length_encoding == STRING_LENGTHS_DIRECT) {
                if (encoded_size64 != (uint64_t) lengths_size) {
                    Rf_error("invalid string length metadata in rdz file");
                }
                lengths = metadata_cursor;
            } else if (length_encoding == STRING_LENGTHS_RLE) {
                R_xlen_t output = 0;
                decoded_lengths = (int32_t *) R_alloc(
                    length == 0 ? 1 : (size_t) length, sizeof(int32_t));
                while (metadata_cursor < metadata_end && output < length) {
                    uint64_t run, code;
                    R_xlen_t j;
                    int32_t value;
                    if (!buffer_varint(&metadata_cursor, metadata_end, &run) ||
                        !buffer_varint(&metadata_cursor, metadata_end, &code) ||
                        run == 0 || run > (uint64_t) (length - output) ||
                        code > (uint64_t) INT_MAX + 1) {
                        Rf_error("invalid string length metadata in rdz file");
                    }
                    value = code == 0 ? -1 : (int32_t) (code - 1);
                    for (j = 0; j < (R_xlen_t) run; ++j) {
                        decoded_lengths[output++] = value;
                    }
                }
                if (output != length || metadata_cursor != metadata_end) {
                    Rf_error("invalid string length metadata in rdz file");
                }
            } else {
                Rf_error("unknown string length encoding in rdz file");
            }
            reader->pos += (size_t) encoded_size64;
        } else {
            reader_need(reader, lengths_size);
            lengths = reader->data + reader->pos;
            reader->pos += lengths_size;
        }
        if (common_encoding == UINT8_MAX) {
            reader_need(reader, (size_t) length);
            encodings = reader->data + reader->pos;
            reader->pos += (size_t) length;
        } else {
            (void) decode_encoding(common_encoding);
        }

        if (layout == STRING_LAYOUT_FLAT_LZ4) {
            if (payload_size64 > (uint64_t) R_XLEN_T_MAX) {
                Rf_error("string vector is too large");
            }
            payload_buffer = PROTECT(Rf_allocVector(
                RAWSXP, (R_xlen_t) payload_size64));
            payload_protected = 1;
            payload = RAW(payload_buffer);
            reader_lz4_blocks(
                reader, (unsigned char *) payload, (size_t) payload_size64,
                "invalid LZ4 string block in rdz file"
            );
        } else {
            reader_need(reader, (size_t) payload_size64);
            payload = reader->data + reader->pos;
        }

        cursor = payload;
        end = payload + (size_t) payload_size64;
        for (i = 0; i < length; ++i) {
            int32_t string_length;
            cetype_t encoding;
            if (decoded_lengths != NULL) {
                string_length = decoded_lengths[i];
            } else {
                memcpy(&string_length, lengths + (size_t) i * sizeof(int32_t),
                       sizeof(string_length));
            }
            if (string_length == -1) {
                SET_STRING_ELT(object, i, NA_STRING);
                continue;
            }
            if (string_length < 0 ||
                (size_t) string_length > (size_t) (end - cursor)) {
                Rf_error("invalid string length in rdz file");
            }
            encoding = decode_encoding(
                encodings == NULL ? common_encoding : encodings[i]);
            SET_STRING_ELT(object, i,
                Rf_mkCharLenCE((const char *) cursor, string_length, encoding));
            cursor += string_length;
        }
        if (cursor != end) Rf_error("invalid string payload in rdz file");
        if (layout == STRING_LAYOUT_FLAT_RAW) {
            reader->pos += (size_t) payload_size64;
        }
        if (payload_protected) UNPROTECT(1);
        return;
    }

    if (layout != 1 && layout != 2 && layout != 4) {
        Rf_error("invalid string layout in rdz file");
    }
    dict_size64 = reader_u64(reader);
    if (dict_size64 > (uint64_t) R_XLEN_T_MAX ||
        dict_size64 > UINT32_MAX) {
        Rf_error("string dictionary is too large");
    }
    dict_size = (R_xlen_t) dict_size64;
    dictionary = PROTECT(Rf_allocVector(STRSXP, dict_size));
    for (i = 0; i < dict_size; ++i) {
        cetype_t encoding = decode_encoding(reader_u8(reader));
        uint64_t string_length64 = reader_u64(reader);
        SEXP string;
        if (string_length64 > INT_MAX) Rf_error("string is too large");
        reader_need(reader, (size_t) string_length64);
        string = Rf_mkCharLenCE((const char *) reader->data + reader->pos,
                                (int) string_length64, encoding);
        SET_STRING_ELT(dictionary, i, string);
        reader->pos += (size_t) string_length64;
    }
    for (i = 0; i < length; ++i) {
        uint64_t index;
        if (layout == 1) {
            index = reader_u8(reader);
        } else if (layout == 2) {
            uint16_t value;
            reader_read(reader, &value, 2);
            index = value;
        } else {
            index = reader_u32(reader);
        }
        if (index > dict_size64) Rf_error("invalid string index in rdz file");
        SET_STRING_ELT(object, i,
                       index == 0 ? NA_STRING : STRING_ELT(dictionary, index - 1));
    }
    UNPROTECT(1);
}

static void decode_logical_v2(reader_t *reader, SEXP object,
                              R_xlen_t length) {
    uint8_t encoding = reader_u8(reader);
    if (encoding == LOGICAL_ENCODING_DIRECT) {
        if ((uint64_t) length > SIZE_MAX / sizeof(int)) {
            Rf_error("logical vector is too large");
        }
        reader_payload(reader, LOGICAL(object), (size_t) length * sizeof(int));
    } else if (encoding == LOGICAL_ENCODING_PACKED) {
        size_t packed_size;
        if (!rdz_logical_packed_size(length, &packed_size)) {
            Rf_error("logical vector is too large");
        }
        reader_need(reader, packed_size);
        if (!rdz_logical_unpack(reader->data + reader->pos, packed_size,
                                    length, LOGICAL(object))) {
            Rf_error("invalid packed logical vector in rdz file");
        }
        reader->pos += packed_size;
    } else {
        Rf_error("unknown logical encoding in rdz file");
    }
}

static void decode_integer_v2(reader_t *reader, SEXP object,
                              R_xlen_t length) {
    uint8_t encoding = reader_u8(reader);
    if (encoding == INTEGER_ENCODING_DIRECT) {
        if ((uint64_t) length > SIZE_MAX / sizeof(int)) {
            Rf_error("integer vector is too large");
        }
        reader_payload(reader, INTEGER(object), (size_t) length * sizeof(int));
    } else if (encoding == INTEGER_ENCODING_PACKED) {
        int32_t base = (int32_t) reader_u32(reader);
        uint8_t bits = reader_u8(reader);
        uint8_t flags = reader_u8(reader);
        size_t packed_size;
        if (!rdz_integer_packed_size(length, bits, &packed_size)) {
            Rf_error("invalid packed integer metadata in rdz file");
        }
        reader_need(reader, packed_size);
        if (!rdz_integer_decode(reader->data + reader->pos, packed_size,
                                    base, bits, flags, length,
                                    INTEGER(object))) {
            Rf_error("invalid packed integer vector in rdz file");
        }
        reader->pos += packed_size;
    } else {
        Rf_error("unknown integer encoding in rdz file");
    }
}

static void decode_real_v2(reader_t *reader, SEXP object, R_xlen_t length) {
    uint8_t encoding = reader_u8(reader);
    if (encoding == REAL_ENCODING_DIRECT) {
        if ((uint64_t) length > SIZE_MAX / sizeof(double)) {
            Rf_error("numeric vector is too large");
        }
        reader_payload(reader, REAL(object), (size_t) length * sizeof(double));
    } else if (encoding == REAL_ENCODING_DICTIONARY ||
               encoding == REAL_ENCODING_DICTIONARY_LZ4) {
        uint8_t bits = reader_u8(reader);
        uint16_t count = reader_u16(reader);
        uint64_t dictionary[RDZ_REAL_DICTIONARY_MAX];
        const unsigned char *packed;
        size_t packed_size;
        if (count == 0 || count > RDZ_REAL_DICTIONARY_MAX ||
            !rdz_real_dictionary_packed_size(length, bits, &packed_size)) {
            Rf_error("invalid numeric dictionary in rdz file");
        }
        reader_read(reader, dictionary, (size_t) count * sizeof(uint64_t));
        if (encoding == REAL_ENCODING_DICTIONARY_LZ4) {
            unsigned char *decoded = (unsigned char *) R_alloc(
                packed_size == 0 ? 1 : packed_size, 1);
            reader_lz4_blocks(
                reader, decoded, packed_size,
                "invalid LZ4 numeric dictionary block in rdz file"
            );
            packed = decoded;
        } else {
            reader_need(reader, packed_size);
            packed = reader->data + reader->pos;
        }
        if (!rdz_real_dictionary_decode(
                packed, packed_size, bits, dictionary, count,
                length, REAL(object))) {
            Rf_error("invalid numeric dictionary indexes in rdz file");
        }
        if (encoding == REAL_ENCODING_DICTIONARY) reader->pos += packed_size;
    } else if (encoding == REAL_ENCODING_SEQUENCE) {
        rdz_real_sequence_t sequence;
        sequence.base = reader_u64(reader);
        sequence.delta = reader_u64(reader);
        if (!rdz_real_sequence_decode(&sequence, length, REAL(object))) {
            Rf_error("invalid numeric sequence in rdz file");
        }
    } else if (encoding == REAL_ENCODING_XOR_LZ4) {
        reader_real_xor_lz4(reader, REAL(object), length);
    } else {
        Rf_error("unknown numeric encoding in rdz file");
    }
}

static SEXP decode_node(reader_t *reader, int depth, int format_version) {
    uint8_t type, object_bit;
    uint64_t length64;
    uint32_t attribute_count;
    SEXPTYPE sexptype;
    size_t element_size = 0;
    R_xlen_t length, i;
    SEXP object;

    if (depth > RDZ_MAX_DEPTH) Rf_error("rdz nesting is too deep");
    type = reader_u8(reader);
    object_bit = reader_u8(reader);
    length64 = reader_u64(reader);
    attribute_count = reader_u32(reader);
    if (length64 > (uint64_t) R_XLEN_T_MAX) Rf_error("vector is too large");
    length = (R_xlen_t) length64;

    switch (type) {
    case NODE_NIL:
        if (length != 0 || attribute_count != 0) Rf_error("invalid NULL node");
        return R_NilValue;
    case NODE_LOGICAL: sexptype = LGLSXP; element_size = sizeof(int); break;
    case NODE_INTEGER: sexptype = INTSXP; element_size = sizeof(int); break;
    case NODE_REAL: sexptype = REALSXP; element_size = sizeof(double); break;
    case NODE_COMPLEX: sexptype = CPLXSXP; element_size = sizeof(Rcomplex); break;
    case NODE_RAW: sexptype = RAWSXP; element_size = sizeof(Rbyte); break;
    case NODE_STRING: sexptype = STRSXP; break;
    case NODE_LIST: sexptype = VECSXP; break;
    default: Rf_error("unknown node type in rdz file");
    }

    object = PROTECT(Rf_allocVector(sexptype, length));
    decode_attributes(reader, object, attribute_count, depth, format_version);
    if (element_size != 0) {
        size_t bytes;
        if (length64 > SIZE_MAX / element_size) Rf_error("vector is too large");
        bytes = (size_t) length64 * element_size;
        switch (type) {
        case NODE_LOGICAL:
            if (format_version >= 2) decode_logical_v2(reader, object, length);
            else reader_payload(reader, LOGICAL(object), bytes);
            break;
        case NODE_INTEGER:
            if (format_version >= 2) decode_integer_v2(reader, object, length);
            else reader_payload(reader, INTEGER(object), bytes);
            break;
        case NODE_REAL:
            if (format_version >= 2) decode_real_v2(reader, object, length);
            else reader_payload(reader, REAL(object), bytes);
            break;
        case NODE_COMPLEX: reader_payload(reader, COMPLEX(object), bytes); break;
        case NODE_RAW: reader_payload(reader, RAW(object), bytes); break;
        default: Rf_error("invalid atomic node type");
        }
    } else if (type == NODE_STRING) {
        decode_strings(reader, object, length, format_version);
    } else {
        for (i = 0; i < length; ++i) {
            SEXP value = PROTECT(decode_node(reader, depth + 1,
                                             format_version));
            SET_VECTOR_ELT(object, i, value);
            UNPROTECT(1);
        }
    }
    SET_OBJECT(object, object_bit != 0);
    UNPROTECT(1);
    return object;
}

static void r_out_char(R_outpstream_t stream, int value) {
    uint8_t byte = (uint8_t) value;
    writer_write((writer_t *) stream->data, &byte, 1);
}

static void r_out_bytes(R_outpstream_t stream, void *data, int length) {
    if (length < 0) Rf_error("negative serialization write length");
    writer_write((writer_t *) stream->data, data, (size_t) length);
}

static int r_in_char(R_inpstream_t stream) {
    reader_t *reader = (reader_t *) stream->data;
    return (int) reader_u8(reader);
}

static void r_in_bytes(R_inpstream_t stream, void *data, int length) {
    if (length < 0) Rf_error("negative serialization read length");
    reader_payload((reader_t *) stream->data, data, (size_t) length);
}

typedef struct {
    writer_t writer;
    SEXP object;
    int codec;
    int format_version;
} save_context_t;

static SEXP save_body(void *data) {
    save_context_t *context = (save_context_t *) data;
    struct R_outpstream_st stream;
    unsigned char architecture[3];

    const unsigned char *magic = context->format_version >= 2 ?
        RDZ_MAGIC_V2 : RDZ_MAGIC_V1;
    writer_write(&context->writer, magic, sizeof(RDZ_MAGIC_V1));
    writer_u8(&context->writer, (uint8_t) context->codec);
    architecture[0] = host_endian();
    architecture[1] = (unsigned char) sizeof(int);
    architecture[2] = (unsigned char) sizeof(double);
    writer_write(&context->writer, architecture, sizeof(architecture));

    if (context->codec == RDZ_CODEC_NATIVE) {
        encode_node(&context->writer, context->object, 0,
                    context->format_version);
    } else {
        R_InitOutPStream(&stream, (R_pstream_data_t) &context->writer,
                         R_pstream_binary_format, 3,
                         r_out_char, r_out_bytes, NULL, R_NilValue);
        R_Serialize(context->object, &stream);
    }
    return R_NilValue;
}

static void save_cleanup(void *data, Rboolean jump) {
    save_context_t *context = (save_context_t *) data;
    (void) jump;
    if (context->writer.file != NULL) {
        if (fflush(context->writer.file) != 0) context->writer.failed = 1;
        if (fclose(context->writer.file) != 0) context->writer.failed = 1;
        context->writer.file = NULL;
    }
}

SEXP C_rdz_save(SEXP object, SEXP path, SEXP mode_sexp, SEXP preset_sexp) {
    const char *filename;
    int mode, preset, supported, codec;
    sexp_stack_t stack;
    save_context_t context;
    SEXP continuation;

    if (TYPEOF(path) != STRSXP || XLENGTH(path) != 1 ||
        STRING_ELT(path, 0) == NA_STRING) {
        Rf_error("`file` must be one non-missing path");
    }
    mode = Rf_asInteger(mode_sexp);
    if (mode < 0 || mode > 2) Rf_error("invalid codec mode");
    preset = Rf_asInteger(preset_sexp);
    if (preset < 0 || preset > 1) Rf_error("invalid rdz preset");
    memset(&stack, 0, sizeof(stack));
    supported = native_supported(object, &stack, 0);
    free(stack.items);
    if (mode == 1 && !supported) {
        Rf_error("object is not supported by the native codec");
    }
    codec = mode == 2 || !supported ? RDZ_CODEC_R : RDZ_CODEC_NATIVE;

    memset(&context, 0, sizeof(context));
    context.object = object;
    context.codec = codec;
    context.format_version = preset == 1 ? 2 : 1;
    filename = CHAR(STRING_ELT(path, 0));
    context.writer.file = fopen(filename, "wb");
    if (context.writer.file == NULL) {
        Rf_error("cannot open '%s' for writing: %s", filename, strerror(errno));
    }
    (void) setvbuf(context.writer.file, NULL, _IOFBF, 1024 * 1024);
    continuation = PROTECT(R_MakeUnwindCont());
    R_UnwindProtect(save_body, &context, save_cleanup, &context, continuation);
    UNPROTECT(1);
    if (context.writer.failed) Rf_error("failed while writing '%s'", filename);
    return R_NilValue;
}

typedef struct {
    reader_t reader;
#ifdef _WIN32
    unsigned char *allocation;
#else
    void *mapping;
#endif
} read_context_t;

static SEXP read_body(void *data) {
    read_context_t *context = (read_context_t *) data;
    reader_t *reader = &context->reader;
    unsigned char magic[sizeof(RDZ_MAGIC_V1)];
    uint8_t codec;
    int format_version;
    unsigned char architecture[3];
    SEXP result;

    reader_read(reader, magic, sizeof(magic));
    if (memcmp(magic, RDZ_MAGIC_V1, sizeof(magic)) == 0) {
        format_version = 1;
    } else if (memcmp(magic, RDZ_MAGIC_V2, sizeof(magic)) == 0) {
        format_version = 2;
    } else {
        Rf_error("not a rdz file");
    }
    codec = reader_u8(reader);
    reader_read(reader, architecture, sizeof(architecture));
    if (architecture[0] != host_endian() ||
        architecture[1] != sizeof(int) || architecture[2] != sizeof(double)) {
        Rf_error("rdz file was written for an incompatible architecture");
    }

    if (codec == RDZ_CODEC_NATIVE) {
        result = decode_node(reader, 0, format_version);
    } else if (codec == RDZ_CODEC_R) {
        struct R_inpstream_st stream;
        R_InitInPStream(&stream, (R_pstream_data_t) reader,
                        R_pstream_any_format, r_in_char, r_in_bytes,
                        NULL, R_NilValue);
        result = R_Unserialize(&stream);
    } else {
        Rf_error("unknown rdz codec");
    }
    if (reader->pos != reader->size) Rf_error("trailing data in rdz file");
    return result;
}

static void read_cleanup(void *data, Rboolean jump) {
    read_context_t *context = (read_context_t *) data;
    (void) jump;
#ifdef _WIN32
    free(context->allocation);
    context->allocation = NULL;
#else
    if (context->mapping != NULL && context->mapping != MAP_FAILED) {
        munmap(context->mapping, context->reader.size);
        context->mapping = NULL;
    }
    if (context->reader.fd >= 0) {
        close(context->reader.fd);
        context->reader.fd = -1;
    }
#endif
}

SEXP C_rdz_read(SEXP path) {
    const char *filename;
    read_context_t context;
    SEXP continuation, result;

    if (TYPEOF(path) != STRSXP || XLENGTH(path) != 1 ||
        STRING_ELT(path, 0) == NA_STRING) {
        Rf_error("`file` must be one non-missing path");
    }
    filename = CHAR(STRING_ELT(path, 0));
    memset(&context, 0, sizeof(context));
#ifndef _WIN32
    context.reader.fd = -1;
#endif

#ifdef _WIN32
    {
        FILE *file = fopen(filename, "rb");
        long size;
        if (file == NULL) Rf_error("cannot open '%s': %s", filename, strerror(errno));
        if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            Rf_error("cannot determine size of '%s'", filename);
        }
        context.allocation = (unsigned char *) malloc((size_t) size);
        if (context.allocation == NULL) {
            fclose(file);
            Rf_error("unable to allocate input buffer");
        }
        if (fread(context.allocation, 1, (size_t) size, file) != (size_t) size) {
            fclose(file);
            free(context.allocation);
            Rf_error("failed while reading '%s'", filename);
        }
        fclose(file);
        context.reader.data = context.allocation;
        context.reader.size = (size_t) size;
    }
#else
    {
        int fd = open(filename, O_RDONLY);
        struct stat status;
        if (fd < 0) Rf_error("cannot open '%s': %s", filename, strerror(errno));
        if (fstat(fd, &status) != 0 || status.st_size <= 0 ||
            (uintmax_t) status.st_size > SIZE_MAX) {
            close(fd);
            Rf_error("cannot determine size of '%s'", filename);
        }
        context.reader.size = (size_t) status.st_size;
        context.mapping = mmap(NULL, context.reader.size, PROT_READ,
                               MAP_PRIVATE, fd, 0);
        if (context.mapping == MAP_FAILED) {
            close(fd);
            Rf_error("cannot map '%s': %s", filename, strerror(errno));
        }
        context.reader.fd = fd;
        context.reader.data = (const unsigned char *) context.mapping;
    }
#endif

    continuation = PROTECT(R_MakeUnwindCont());
    result = R_UnwindProtect(read_body, &context, read_cleanup, &context,
                             continuation);
    PROTECT(result);
    UNPROTECT(2);
    return result;
}
