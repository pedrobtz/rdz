#include <string.h>

#include <zubin/layout.h>
#include <zubin/rw.h>
#include <zufast/hash.h>

#include "rdz_records.h"

uint64_t rdz_hash(const void *data, size_t n)
{
    return zuf_hash64(data, n);
}

void rdz_object_encode(uint8_t *out, const rdz_object *o)
{
    memset(out, 0, RDZ_OBJECT_ENTRY_LEN);
    zb_wr_u32le(out + RDZ_OE_OBJECT_ID, o->object_id);
    zb_wr_u32le(out + RDZ_OE_PARENT_ID, o->parent_id);
    zb_wr_u16le(out + RDZ_OE_ROLE, o->role);
    zb_wr_u16le(out + RDZ_OE_TYPE, o->type_tag);
    zb_wr_u32le(out + RDZ_OE_FLAGS, o->flags);
    zb_wr_u64le(out + RDZ_OE_LOGICAL_LEN, o->logical_len);
    zb_wr_u32le(out + RDZ_OE_FIRST_CHILD, o->first_child);
    zb_wr_u32le(out + RDZ_OE_CHILD_COUNT, o->child_count);
    zb_wr_u32le(out + RDZ_OE_FIRST_ATTRIBUTE, o->first_attribute);
    zb_wr_u32le(out + RDZ_OE_ATTRIBUTE_COUNT, o->attribute_count);
    zb_wr_u32le(out + RDZ_OE_FIRST_BLOCK, o->first_block);
    zb_wr_u32le(out + RDZ_OE_BLOCK_COUNT, o->block_count);
}

void rdz_object_decode(const uint8_t *in, rdz_object *o)
{
    o->object_id = zb_rd_u32le(in + RDZ_OE_OBJECT_ID);
    o->parent_id = zb_rd_u32le(in + RDZ_OE_PARENT_ID);
    o->role = zb_rd_u16le(in + RDZ_OE_ROLE);
    o->type_tag = zb_rd_u16le(in + RDZ_OE_TYPE);
    o->flags = zb_rd_u32le(in + RDZ_OE_FLAGS);
    o->logical_len = zb_rd_u64le(in + RDZ_OE_LOGICAL_LEN);
    o->first_child = zb_rd_u32le(in + RDZ_OE_FIRST_CHILD);
    o->child_count = zb_rd_u32le(in + RDZ_OE_CHILD_COUNT);
    o->first_attribute = zb_rd_u32le(in + RDZ_OE_FIRST_ATTRIBUTE);
    o->attribute_count = zb_rd_u32le(in + RDZ_OE_ATTRIBUTE_COUNT);
    o->first_block = zb_rd_u32le(in + RDZ_OE_FIRST_BLOCK);
    o->block_count = zb_rd_u32le(in + RDZ_OE_BLOCK_COUNT);
}

void rdz_attribute_encode(uint8_t *out, const rdz_attribute *a)
{
    memset(out, 0, RDZ_ATTRIBUTE_ENTRY_LEN);
    zb_wr_u32le(out + RDZ_AE_OWNER_ID, a->owner_id);
    zb_wr_u32le(out + RDZ_AE_NAME_OBJECT_ID, a->name_object_id);
    zb_wr_u32le(out + RDZ_AE_VALUE_OBJECT_ID, a->value_object_id);
    zb_wr_u32le(out + RDZ_AE_ORDINAL, a->ordinal);
    zb_wr_u32le(out + RDZ_AE_FLAGS, a->flags);
}

int rdz_attribute_decode(const uint8_t *in, rdz_attribute *a)
{
    a->owner_id = zb_rd_u32le(in + RDZ_AE_OWNER_ID);
    a->name_object_id = zb_rd_u32le(in + RDZ_AE_NAME_OBJECT_ID);
    a->value_object_id = zb_rd_u32le(in + RDZ_AE_VALUE_OBJECT_ID);
    a->ordinal = zb_rd_u32le(in + RDZ_AE_ORDINAL);
    a->flags = zb_rd_u32le(in + RDZ_AE_FLAGS);
    return zb_rd_u32le(in + RDZ_AE_RESERVED) != 0 || zb_rd_u64le(in + RDZ_AE_RESERVED_WIDE) != 0;
}

void rdz_block_entry_encode(uint8_t *out, const rdz_block *b)
{
    memset(out, 0, RDZ_BLOCK_ENTRY_LEN);
    zb_wr_u32le(out + RDZ_BE_SEQUENCE, b->sequence);
    zb_wr_u32le(out + RDZ_BE_FLAGS, b->flags);
    zb_wr_u64le(out + RDZ_BE_HEADER_OFFSET, b->header_offset);
    zb_wr_u64le(out + RDZ_BE_PAYLOAD_OFFSET, b->payload_offset);
    zb_wr_u32le(out + RDZ_BE_STORED_LEN, b->stored_len);
    zb_wr_u64le(out + RDZ_BE_LOGICAL_COUNT, b->logical_count);
    zb_wr_u64le(out + RDZ_BE_DECODED_LEN, b->decoded_len);
    zb_wr_u16le(out + RDZ_BE_ENCODING, b->encoding);
    zb_wr_u16le(out + RDZ_BE_COMPRESSION, b->compression);
    zb_wr_u64le(out + RDZ_BE_CHECKSUM, b->checksum);
}

int rdz_block_entry_decode(const uint8_t *in, rdz_block *b)
{
    b->sequence = zb_rd_u32le(in + RDZ_BE_SEQUENCE);
    b->flags = zb_rd_u32le(in + RDZ_BE_FLAGS);
    b->header_offset = zb_rd_u64le(in + RDZ_BE_HEADER_OFFSET);
    b->payload_offset = zb_rd_u64le(in + RDZ_BE_PAYLOAD_OFFSET);
    b->stored_len = zb_rd_u32le(in + RDZ_BE_STORED_LEN);
    b->logical_count = zb_rd_u64le(in + RDZ_BE_LOGICAL_COUNT);
    b->decoded_len = zb_rd_u64le(in + RDZ_BE_DECODED_LEN);
    b->encoding = zb_rd_u16le(in + RDZ_BE_ENCODING);
    b->compression = zb_rd_u16le(in + RDZ_BE_COMPRESSION);
    b->checksum = zb_rd_u64le(in + RDZ_BE_CHECKSUM);
    return zb_rd_u32le(in + RDZ_BE_RESERVED) != 0 || zb_rd_u32le(in + RDZ_BE_RESERVED_TAIL) != 0;
}

void rdz_block_header_encode(uint8_t *out, const rdz_block *b)
{
    memset(out, 0, RDZ_BLOCK_HEADER_LEN);
    memcpy(out + RDZ_BH_MAGIC, RDZ_BLOCK_MAGIC, 4);
    zb_wr_u16le(out + RDZ_BH_HEADER_LEN, (uint16_t)RDZ_BLOCK_HEADER_LEN);
    zb_wr_u32le(out + RDZ_BH_SEQUENCE, b->sequence);
    zb_wr_u16le(out + RDZ_BH_ENCODING, b->encoding);
    zb_wr_u16le(out + RDZ_BH_COMPRESSION, b->compression);
    zb_wr_u64le(out + RDZ_BH_LOGICAL_COUNT, b->logical_count);
    zb_wr_u64le(out + RDZ_BH_DECODED_LEN, b->decoded_len);
    zb_wr_u32le(out + RDZ_BH_STORED_LEN, b->stored_len);
    zb_wr_u64le(out + RDZ_BH_CHECKSUM, b->checksum);
}

typedef struct {
    const char *name;
    const char *spec;
    uint32_t size;
    const uint32_t *offsets;
    uint32_t noffsets;
} rdz_record_def;

#define RDZ_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

int rdz_records_check(const char **record)
{
    static const uint32_t fh[] = {
        RDZ_FH_MAGIC, RDZ_FH_VERSION, RDZ_FH_HEADER_LEN, RDZ_FH_FLAGS, RDZ_FH_CODEC,
        RDZ_FH_CODEC_VERSION, RDZ_FH_MAX_BLOCK, RDZ_FH_WRITER, RDZ_FH_CHECKSUM};
    static const uint32_t bh[] = {
        RDZ_BH_MAGIC, RDZ_BH_HEADER_LEN, RDZ_BH_FLAGS, RDZ_BH_SEQUENCE, RDZ_BH_ENCODING,
        RDZ_BH_COMPRESSION, RDZ_BH_LOGICAL_COUNT, RDZ_BH_DECODED_LEN, RDZ_BH_STORED_LEN,
        RDZ_BH_RESERVED, RDZ_BH_CHECKSUM};
    static const uint32_t dh[] = {
        RDZ_DH_MAGIC, RDZ_DH_VERSION, RDZ_DH_HEADER_LEN, RDZ_DH_OBJECT_WIDTH,
        RDZ_DH_ATTRIBUTE_WIDTH, RDZ_DH_BLOCK_WIDTH, RDZ_DH_FLAGS, RDZ_DH_OBJECTS,
        RDZ_DH_ATTRIBUTES, RDZ_DH_BLOCKS, RDZ_DH_SYNOPSIS_LEN, RDZ_DH_CHECKSUM};
    static const uint32_t oe[] = {
        RDZ_OE_OBJECT_ID, RDZ_OE_PARENT_ID, RDZ_OE_ROLE, RDZ_OE_TYPE, RDZ_OE_FLAGS,
        RDZ_OE_LOGICAL_LEN, RDZ_OE_FIRST_CHILD, RDZ_OE_CHILD_COUNT, RDZ_OE_FIRST_ATTRIBUTE,
        RDZ_OE_ATTRIBUTE_COUNT, RDZ_OE_FIRST_BLOCK, RDZ_OE_BLOCK_COUNT};
    static const uint32_t ae[] = {
        RDZ_AE_OWNER_ID, RDZ_AE_NAME_OBJECT_ID, RDZ_AE_VALUE_OBJECT_ID, RDZ_AE_ORDINAL,
        RDZ_AE_FLAGS, RDZ_AE_RESERVED, RDZ_AE_RESERVED_WIDE};
    static const uint32_t be[] = {
        RDZ_BE_SEQUENCE, RDZ_BE_FLAGS, RDZ_BE_HEADER_OFFSET, RDZ_BE_PAYLOAD_OFFSET,
        RDZ_BE_STORED_LEN, RDZ_BE_RESERVED, RDZ_BE_LOGICAL_COUNT, RDZ_BE_DECODED_LEN,
        RDZ_BE_ENCODING, RDZ_BE_COMPRESSION, RDZ_BE_RESERVED_TAIL, RDZ_BE_CHECKSUM};
    static const uint32_t tr[] = {
        RDZ_TR_MAGIC, RDZ_TR_VERSION, RDZ_TR_TRAILER_LEN, RDZ_TR_DIRECTORY_OFFSET,
        RDZ_TR_DIRECTORY_LEN, RDZ_TR_CHECKSUM, RDZ_TR_RESERVED, RDZ_TR_CLOSING};
    const rdz_record_def defs[] = {
        {"file header", RDZ_SPEC_FILE_HEADER, RDZ_HEADER_LEN, fh, RDZ_COUNT(fh)},
        {"block header", RDZ_SPEC_BLOCK_HEADER, RDZ_BLOCK_HEADER_LEN, bh, RDZ_COUNT(bh)},
        {"directory header", RDZ_SPEC_DIRECTORY_HEADER, RDZ_DIRECTORY_HEADER_LEN, dh,
         RDZ_COUNT(dh)},
        {"object entry", RDZ_SPEC_OBJECT_ENTRY, RDZ_OBJECT_ENTRY_LEN, oe, RDZ_COUNT(oe)},
        {"attribute entry", RDZ_SPEC_ATTRIBUTE_ENTRY, RDZ_ATTRIBUTE_ENTRY_LEN, ae,
         RDZ_COUNT(ae)},
        {"block entry", RDZ_SPEC_BLOCK_ENTRY, RDZ_BLOCK_ENTRY_LEN, be, RDZ_COUNT(be)},
        {"trailer", RDZ_SPEC_TRAILER, RDZ_TRAILER_LEN, tr, RDZ_COUNT(tr)}};
    zb_field fields[16];
    uint32_t i, k;
    for (i = 0; i < RDZ_COUNT(defs); i++) {
        zb_layout layout;
        size_t err_pos = 0;
        const rdz_record_def *d = &defs[i];
        *record = d->name;
        if (zb_layout_parse(d->spec, strlen(d->spec), 0, 0, fields, 16, &layout, &err_pos) ||
            layout.size != d->size || layout.nfields != d->noffsets) {
            return (int)i + 1;
        }
        for (k = 0; k < layout.nfields; k++) {
            if (fields[k].offset != d->offsets[k] || fields[k].big_endian) return (int)i + 1;
        }
    }
    *record = NULL;
    return 0;
}
