/*
 * rdz_records.h -- the seven fixed records of container version 3.
 *
 * Each record is a zubin layout specification; the RDZ_<record>_<field>
 * offsets below are what zb_layout_parse() derives from it, and
 * rdz_records_check() proves that by parsing every specification and
 * comparing (the C harness and the R test suite both call it). Records are
 * read and written with zubin's byte-order-explicit zb_rd_ / zb_wr_ at those
 * offsets, so the bytes are the same on every host.
 */
#ifndef RDZ_RECORDS_H
#define RDZ_RECORDS_H

#include "rdz_format.h"

#define RDZ_SPEC_FILE_HEADER \
    "<magic:b4 version:u16 header_len:u16 flags:u32 codec:u16 codec_version:u16 " \
    "max_block:u32 reserved:u32 checksum:u64"
#define RDZ_SPEC_BLOCK_HEADER \
    "<magic:b4 header_len:u16 flags:u16 sequence:u32 encoding:u16 compression:u16 " \
    "logical_count:u64 decoded_len:u64 stored_len:u32 reserved:u32 checksum:u64"
#define RDZ_SPEC_DIRECTORY_HEADER \
    "<magic:b4 version:u16 header_len:u16 object_width:u16 attribute_width:u16 " \
    "block_width:u16 flags:u16 objects:u32 attributes:u32 blocks:u32 synopsis_len:u32 " \
    "checksum:u64"
#define RDZ_SPEC_OBJECT_ENTRY \
    "<object_id:u32 parent_id:u32 role:u16 type:u16 flags:u32 logical_len:u64 " \
    "first_child:u32 child_count:u32 first_attribute:u32 attribute_count:u32 " \
    "first_block:u32 block_count:u32"
#define RDZ_SPEC_ATTRIBUTE_ENTRY \
    "<owner_id:u32 name_object_id:u32 value_object_id:u32 ordinal:u32 flags:u32 " \
    "reserved:u32 reserved_wide:u64"
#define RDZ_SPEC_BLOCK_ENTRY \
    "<sequence:u32 flags:u32 header_offset:u64 payload_offset:u64 stored_len:u32 " \
    "reserved:u32 logical_count:u64 decoded_len:u64 encoding:u16 compression:u16 " \
    "reserved_tail:u32 checksum:u64"
#define RDZ_SPEC_TRAILER \
    "<magic:b4 version:u16 trailer_len:u16 directory_offset:u64 directory_len:u64 " \
    "checksum:u64 reserved:u32 closing:b4"

enum {
    RDZ_FH_MAGIC = 0, RDZ_FH_VERSION = 4, RDZ_FH_HEADER_LEN = 6, RDZ_FH_FLAGS = 8,
    RDZ_FH_CODEC = 12, RDZ_FH_CODEC_VERSION = 14, RDZ_FH_MAX_BLOCK = 16,
    RDZ_FH_RESERVED = 20, RDZ_FH_CHECKSUM = 24
};
enum {
    RDZ_BH_MAGIC = 0, RDZ_BH_HEADER_LEN = 4, RDZ_BH_FLAGS = 6, RDZ_BH_SEQUENCE = 8,
    RDZ_BH_ENCODING = 12, RDZ_BH_COMPRESSION = 14, RDZ_BH_LOGICAL_COUNT = 16,
    RDZ_BH_DECODED_LEN = 24, RDZ_BH_STORED_LEN = 32, RDZ_BH_RESERVED = 36,
    RDZ_BH_CHECKSUM = 40
};
enum {
    RDZ_DH_MAGIC = 0, RDZ_DH_VERSION = 4, RDZ_DH_HEADER_LEN = 6, RDZ_DH_OBJECT_WIDTH = 8,
    RDZ_DH_ATTRIBUTE_WIDTH = 10, RDZ_DH_BLOCK_WIDTH = 12, RDZ_DH_FLAGS = 14,
    RDZ_DH_OBJECTS = 16, RDZ_DH_ATTRIBUTES = 20, RDZ_DH_BLOCKS = 24,
    RDZ_DH_SYNOPSIS_LEN = 28, RDZ_DH_CHECKSUM = 32
};
enum {
    RDZ_OE_OBJECT_ID = 0, RDZ_OE_PARENT_ID = 4, RDZ_OE_ROLE = 8, RDZ_OE_TYPE = 10,
    RDZ_OE_FLAGS = 12, RDZ_OE_LOGICAL_LEN = 16, RDZ_OE_FIRST_CHILD = 24,
    RDZ_OE_CHILD_COUNT = 28, RDZ_OE_FIRST_ATTRIBUTE = 32, RDZ_OE_ATTRIBUTE_COUNT = 36,
    RDZ_OE_FIRST_BLOCK = 40, RDZ_OE_BLOCK_COUNT = 44
};
enum {
    RDZ_AE_OWNER_ID = 0, RDZ_AE_NAME_OBJECT_ID = 4, RDZ_AE_VALUE_OBJECT_ID = 8,
    RDZ_AE_ORDINAL = 12, RDZ_AE_FLAGS = 16, RDZ_AE_RESERVED = 20, RDZ_AE_RESERVED_WIDE = 24
};
enum {
    RDZ_BE_SEQUENCE = 0, RDZ_BE_FLAGS = 4, RDZ_BE_HEADER_OFFSET = 8,
    RDZ_BE_PAYLOAD_OFFSET = 16, RDZ_BE_STORED_LEN = 24, RDZ_BE_RESERVED = 28,
    RDZ_BE_LOGICAL_COUNT = 32, RDZ_BE_DECODED_LEN = 40, RDZ_BE_ENCODING = 48,
    RDZ_BE_COMPRESSION = 50, RDZ_BE_RESERVED_TAIL = 52, RDZ_BE_CHECKSUM = 56
};
enum {
    RDZ_TR_MAGIC = 0, RDZ_TR_VERSION = 4, RDZ_TR_TRAILER_LEN = 6,
    RDZ_TR_DIRECTORY_OFFSET = 8, RDZ_TR_DIRECTORY_LEN = 16, RDZ_TR_CHECKSUM = 24,
    RDZ_TR_RESERVED = 32, RDZ_TR_CLOSING = 36
};

typedef struct {
    uint32_t object_id, parent_id;
    uint16_t role, type_tag;
    uint32_t flags;
    uint64_t logical_len;
    uint32_t first_child, child_count, first_attribute, attribute_count;
    uint32_t first_block, block_count;
} rdz_object;

typedef struct {
    uint32_t owner_id, name_object_id, value_object_id, ordinal, flags;
} rdz_attribute;

typedef struct {
    uint32_t sequence, flags;
    uint64_t header_offset, payload_offset;
    uint32_t stored_len;
    uint64_t logical_count, decoded_len;
    uint16_t encoding, compression;
    uint64_t checksum;
} rdz_block;

void rdz_object_encode(uint8_t *out, const rdz_object *o);
void rdz_object_decode(const uint8_t *in, rdz_object *o);
void rdz_attribute_encode(uint8_t *out, const rdz_attribute *a);
/* Nonzero when a reserved field is set. */
int rdz_attribute_decode(const uint8_t *in, rdz_attribute *a);
void rdz_block_entry_encode(uint8_t *out, const rdz_block *b);
/* Nonzero when a reserved field is set. */
int rdz_block_entry_decode(const uint8_t *in, rdz_block *b);
void rdz_block_header_encode(uint8_t *out, const rdz_block *b);

uint64_t rdz_hash(const void *data, size_t n);

/* Parses every specification and compares its size and offsets with the
   constants above. Returns 0, or the 1-based number of the first record
   that disagrees, with its name in *record. */
int rdz_records_check(const char **record);

#endif /* RDZ_RECORDS_H */
