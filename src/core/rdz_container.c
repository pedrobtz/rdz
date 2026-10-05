#include <string.h>

#include <zubin/rw.h>

#include "rdz_container.h"

/* ---- validation helpers --------------------------------------------------------- */

static int rdz_logical_encoding(uint16_t encoding)
{
    switch (encoding) {
    case RDZ_ENCODING_LOGICAL_2BIT:
    case RDZ_ENCODING_LOGICAL_CONSTANT:
    case RDZ_ENCODING_LOGICAL_DENSE_PLANES:
    case RDZ_ENCODING_LOGICAL_SPARSE_PATCHES:
    case RDZ_ENCODING_LOGICAL_RUN_ENDS:
    case RDZ_ENCODING_LOGICAL_PERIODIC:
        return 1;
    default:
        return 0;
    }
}

/* The stored length each logical record allows for `count` values
   (logical.rs validate_encoded_length). count is at most 65,536 here. */
static int rdz_logical_length_ok(uint16_t encoding, uint64_t count, uint64_t stored)
{
    uint64_t bitmap = (count + 7) / 8;
    switch (encoding) {
    case RDZ_ENCODING_LOGICAL_2BIT:
        return stored == (count + 3) / 4;
    case RDZ_ENCODING_LOGICAL_CONSTANT:
        return count != 0 && stored == RDZ_LOGICAL_CONSTANT_HEADER_LEN;
    case RDZ_ENCODING_LOGICAL_DENSE_PLANES:
        if (count == 0) return stored == RDZ_LOGICAL_DENSE_HEADER_LEN;
        return stored >= RDZ_LOGICAL_DENSE_HEADER_LEN + bitmap &&
               stored <= RDZ_LOGICAL_DENSE_HEADER_LEN + 2 * bitmap;
    case RDZ_ENCODING_LOGICAL_SPARSE_PATCHES:
        return count != 0 && count <= 65536 && stored >= RDZ_LOGICAL_SPARSE_HEADER_LEN + 2 &&
               stored <= RDZ_LOGICAL_SPARSE_HEADER_LEN + count * 2;
    case RDZ_ENCODING_LOGICAL_RUN_ENDS:
        return count != 0 &&
               stored >= RDZ_LOGICAL_RUN_HEADER_LEN + RDZ_LOGICAL_RUN_RECORD_LEN &&
               stored <= RDZ_LOGICAL_RUN_HEADER_LEN + count * RDZ_LOGICAL_RUN_RECORD_LEN;
    case RDZ_ENCODING_LOGICAL_PERIODIC:
        return count >= 8 && stored >= RDZ_LOGICAL_PERIODIC_HEADER_LEN + 1 &&
               stored <= RDZ_LOGICAL_PERIODIC_HEADER_LEN + (RDZ_LOGICAL_MAX_PERIOD + 3) / 4;
    default:
        return 0;
    }
}

/* [first, first + count) within nblocks. */
static int rdz_block_range(const rdz_object *o, uint32_t nblocks, uint32_t *end, rdz_error *e)
{
    if (o->first_block > UINT32_MAX - o->block_count) {
        return rdz_invalid(e, "block range overflow");
    }
    *end = o->first_block + o->block_count;
    if (*end > nblocks) return rdz_invalid(e, "object block range is out of bounds");
    return 0;
}

static int rdz_check_object(const rdz_object *o, uint32_t id, uint32_t parent, uint16_t role,
                            uint16_t type_tag, rdz_error *e)
{
    if (o->object_id != id || o->parent_id != parent || o->role != role ||
        o->type_tag != type_tag || o->flags != 0) {
        return rdz_invalid(e, "invalid native object descriptor");
    }
    return 0;
}

static int rdz_check_leaf(const rdz_object *o, rdz_error *e)
{
    if (o->first_child != 0 || o->child_count != 0 || o->first_attribute != 0 ||
        o->attribute_count != 0) {
        return rdz_invalid(e, "attribute object is not a leaf");
    }
    return 0;
}

static int rdz_check_logical_blocks(const rdz_object *o, const rdz_block *blocks,
                                    uint32_t nblocks, rdz_error *e)
{
    uint32_t i, end;
    uint64_t total = 0;
    if (rdz_block_range(o, nblocks, &end, e)) return 1;
    for (i = o->first_block; i < end; i++) {
        const rdz_block *b = &blocks[i];
        uint64_t count = b->logical_count;
        if (!rdz_logical_encoding(b->encoding)) {
            return rdz_invalid(e, "unexpected logical block encoding");
        }
        if (count > RDZ_LOGICAL_BLOCK_VALUES || (o->logical_len != 0 && count == 0) ||
            (i + 1 < end && count != RDZ_LOGICAL_BLOCK_VALUES)) {
            return rdz_invalid(e, "non-canonical logical block size");
        }
        if (total > UINT64_MAX - count) return rdz_invalid(e, "logical object length overflow");
        total += count;
    }
    if (total != o->logical_len) return rdz_invalid(e, "logical object length mismatch");
    for (i = o->first_block; i < end; i++) {
        if (!rdz_logical_length_ok(blocks[i].encoding, blocks[i].logical_count,
                                   blocks[i].decoded_len)) {
            return rdz_invalid(e, "logical block encoding length mismatch");
        }
    }
    return 0;
}

static int rdz_check_object_blocks(const rdz_object *o, const rdz_block *blocks,
                                   uint32_t nblocks, uint16_t encoding, rdz_error *e)
{
    uint32_t i, end;
    uint64_t total = 0;
    if (o->block_count == 0) return rdz_invalid(e, "object has no data block");
    if (rdz_block_range(o, nblocks, &end, e)) return 1;
    for (i = o->first_block; i < end; i++) {
        if (blocks[i].encoding != encoding) {
            return rdz_invalid(e, "object block encoding mismatch");
        }
        if (total > UINT64_MAX - blocks[i].logical_count) {
            return rdz_limit(e, "object logical length");
        }
        total += blocks[i].logical_count;
    }
    if (total != o->logical_len) return rdz_invalid(e, "object logical length mismatch");
    return 0;
}

/* A character object may mix plain blocks with dictionary entry and index
   blocks; entry blocks do not count toward its length, and their total
   cannot exceed it, which bounds the dictionary a reader allocates. */
static int rdz_check_string_blocks(const rdz_object *o, const rdz_block *blocks,
                                   uint32_t nblocks, rdz_error *e)
{
    uint32_t i, end;
    uint64_t elements = 0, entries = 0;
    if (o->block_count == 0) return rdz_invalid(e, "object has no data block");
    if (rdz_block_range(o, nblocks, &end, e)) return 1;
    for (i = o->first_block; i < end; i++) {
        const rdz_block *b = &blocks[i];
        uint64_t count = b->logical_count;
        switch (b->encoding) {
        case RDZ_ENCODING_STRING_PLAIN:
            if (elements > UINT64_MAX - count) return rdz_limit(e, "object logical length");
            elements += count;
            break;
        case RDZ_ENCODING_STRING_DICT_ENTRIES:
            if (entries > UINT64_MAX - count) return rdz_limit(e, "dictionary length");
            entries += count;
            break;
        case RDZ_ENCODING_STRING_DICT_INDICES: {
            static const uint64_t widths[] = {1, 2, 4};
            int k, ok = 0;
            for (k = 0; k < 3; k++) {
                uint64_t w = widths[k];
                if (count <= (UINT64_MAX - RDZ_DICT_INDEX_HEADER_LEN) / w &&
                    count * w + RDZ_DICT_INDEX_HEADER_LEN == b->decoded_len) {
                    ok = 1;
                }
            }
            if (!ok) return rdz_invalid(e, "dictionary index block length mismatch");
            if (elements > UINT64_MAX - count) return rdz_limit(e, "object logical length");
            elements += count;
            break;
        }
        default:
            return rdz_invalid(e, "object block encoding mismatch");
        }
    }
    if (elements != o->logical_len) return rdz_invalid(e, "object logical length mismatch");
    if (entries > o->logical_len) {
        return rdz_invalid(e, "string dictionary is larger than its vector");
    }
    return 0;
}

static int rdz_check_generic_schema(const rdz_reader *r, rdz_error *e)
{
    uint32_t i;
    if (r->nobjects != 0 || r->nattributes != 0) {
        return rdz_invalid(e, "generic codec cannot contain native object entries");
    }
    for (i = 0; i < r->nblocks; i++) {
        const rdz_block *b = &r->blocks[i];
        if (b->encoding != RDZ_ENCODING_RAW || b->logical_count != b->decoded_len) {
            return rdz_invalid(e, "invalid generic raw block");
        }
    }
    return 0;
}

static int rdz_check_native_logical_schema(const rdz_reader *r, rdz_error *e)
{
    const rdz_object *root, *name, *value;
    const rdz_attribute *a;
    uint32_t root_end, name_end, value_end;
    if (r->synopsis_len != 0) {
        return rdz_invalid(e, "native codec cannot contain a generic synopsis");
    }
    if (!((r->nobjects == 1 && r->nattributes == 0) || (r->nobjects == 3 && r->nattributes == 1))) {
        return rdz_invalid(e, "invalid logical object directory shape");
    }
    root = &r->objects[0];
    if (rdz_check_object(root, 0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_LOGICAL, e)) return 1;
    if (root->first_child != 0 || root->child_count != 0 || root->first_attribute != 0) {
        return rdz_invalid(e, "invalid logical root references");
    }
    if (root->attribute_count != r->nattributes) {
        return rdz_invalid(e, "logical attribute count mismatch");
    }
    if (rdz_check_logical_blocks(root, r->blocks, r->nblocks, e)) return 1;
    if (r->nattributes == 0) {
        if (root->first_block != 0 || root->block_count != r->nblocks) {
            return rdz_invalid(e, "logical blocks are not fully indexed");
        }
        return 0;
    }
    name = &r->objects[1];
    value = &r->objects[2];
    if (rdz_check_object(name, 1, 0, RDZ_ROLE_ATTRIBUTE_NAME, RDZ_TYPE_CHARACTER, e)) return 1;
    if (rdz_check_object(value, 2, 0, RDZ_ROLE_ATTRIBUTE_VALUE, RDZ_TYPE_CHARACTER, e)) return 1;
    if (name->logical_len != 1 || value->logical_len != root->logical_len) {
        return rdz_invalid(e, "names object length mismatch");
    }
    if (rdz_check_leaf(name, e) || rdz_check_leaf(value, e)) return 1;
    if (rdz_check_object_blocks(name, r->blocks, r->nblocks, RDZ_ENCODING_STRING_PLAIN, e)) return 1;
    if (rdz_check_string_blocks(value, r->blocks, r->nblocks, e)) return 1;
    a = &r->attributes[0];
    if (a->owner_id != 0 || a->name_object_id != 1 || a->value_object_id != 2 ||
        a->ordinal != 0 || a->flags != RDZ_ATTRIBUTE_FLAG_NAMES) {
        return rdz_invalid(e, "invalid names attribute entry");
    }
    /* The ranges were checked against nblocks above, so these cannot wrap. */
    root_end = root->first_block + root->block_count;
    name_end = name->first_block + name->block_count;
    value_end = value->first_block + value->block_count;
    if (root->first_block != 0 || name->first_block != root_end ||
        value->first_block != name_end || value_end != r->nblocks) {
        return rdz_invalid(e, "native object blocks are not canonical");
    }
    return 0;
}

/* ---- reader ------------------------------------------------------------------------ */

void rdz_reader_init(rdz_reader *r)
{
    memset(r, 0, sizeof *r);
    zb_buf_init(&r->directory);
    zb_buf_init(&r->tables);
    /* growable, empty: allocating nothing, this cannot fail */
    zb_buf_alloc(&r->scratch, 0, (size_t)RDZ_MAX_BLOCK_SIZE);
    rdz_codec_init(&r->codec);
    r->file.fp = NULL;
    r->file.mem = NULL;
}

void rdz_reader_close(rdz_reader *r)
{
    rdz_infile_close(&r->file);
    zb_buf_release(&r->directory);
    zb_buf_release(&r->tables);
    zb_buf_release(&r->scratch);
    rdz_codec_free(&r->codec);
    r->objects = NULL;
    r->attributes = NULL;
    r->blocks = NULL;
    r->synopsis = NULL;
}

static int rdz_check_codec(uint16_t id, uint16_t version, rdz_error *e)
{
    if ((id == RDZ_CODEC_R_SERIAL_V3 && version == RDZ_R_SERIAL_CODEC_VERSION) ||
        (id == RDZ_CODEC_NATIVE_V1 && version == RDZ_NATIVE_CODEC_VERSION)) {
        return 0;
    }
    return rdz_codec_error(e, id, version);
}

static int rdz_parse_directory(rdz_reader *r, rdz_error *e)
{
    const uint8_t *d = r->directory.data;
    size_t len = r->directory.len, offset, expected, tables_len;
    uint32_t nobj, natt, nblk, i;
    uint64_t synopsis_len, next_block = RDZ_HEADER_LEN;
    rdz_block *blocks;
    rdz_object *objects;
    rdz_attribute *attributes;

    if (len < RDZ_DIRECTORY_HEADER_LEN || memcmp(d, RDZ_DIRECTORY_MAGIC, 4) != 0) {
        return rdz_invalid(e, "invalid directory header");
    }
    if (zb_rd_u16le(d + RDZ_DH_VERSION) != RDZ_DIRECTORY_VERSION ||
        zb_rd_u16le(d + RDZ_DH_HEADER_LEN) != RDZ_DIRECTORY_HEADER_LEN ||
        zb_rd_u16le(d + RDZ_DH_OBJECT_WIDTH) != RDZ_OBJECT_ENTRY_LEN ||
        zb_rd_u16le(d + RDZ_DH_ATTRIBUTE_WIDTH) != RDZ_ATTRIBUTE_ENTRY_LEN ||
        zb_rd_u16le(d + RDZ_DH_BLOCK_WIDTH) != RDZ_BLOCK_ENTRY_LEN) {
        return rdz_invalid(e, "unsupported directory version or entry size");
    }
    if (zb_rd_u16le(d + RDZ_DH_FLAGS) != 0) return rdz_invalid(e, "unsupported directory flags");
    nobj = zb_rd_u32le(d + RDZ_DH_OBJECTS);
    natt = zb_rd_u32le(d + RDZ_DH_ATTRIBUTES);
    nblk = zb_rd_u32le(d + RDZ_DH_BLOCKS);
    synopsis_len = zb_rd_u32le(d + RDZ_DH_SYNOPSIS_LEN);
    if (nobj > RDZ_MAX_OBJECTS) return rdz_limit(e, "object count");
    if (natt > RDZ_MAX_ATTRIBUTES) return rdz_limit(e, "attribute count");
    if (nblk == 0 || nblk > RDZ_MAX_BLOCKS) return rdz_limit(e, "block count");
    if (synopsis_len > RDZ_MAX_SYNOPSIS_LEN) return rdz_limit(e, "synopsis length");
    if (RDZ_CHECKSUM_DIFFERS(zb_rd_u64le(d + RDZ_DH_CHECKSUM), rdz_hash(d, RDZ_DH_CHECKSUM))) {
        return rdz_invalid(e, "directory header checksum mismatch");
    }
    /* The counts are bounded above, so none of these sizes can wrap. */
    expected = RDZ_DIRECTORY_HEADER_LEN + (size_t)nobj * RDZ_OBJECT_ENTRY_LEN +
               (size_t)natt * RDZ_ATTRIBUTE_ENTRY_LEN + (size_t)nblk * RDZ_BLOCK_ENTRY_LEN +
               (size_t)synopsis_len;
    if (expected != len) return rdz_invalid(e, "directory length mismatch");

    tables_len = (size_t)nblk * sizeof(rdz_block) + (size_t)nobj * sizeof(rdz_object) +
                 (size_t)natt * sizeof(rdz_attribute);
    if (zb_buf_alloc(&r->tables, tables_len, 0) || !zb_put_raw(&r->tables, tables_len)) {
        return rdz_memory(e, "the directory tables");
    }
    /* blocks, then objects, then attributes: each a multiple of the next's
       alignment */
    blocks = (rdz_block *)(void *)r->tables.data;
    objects = (rdz_object *)(void *)(blocks + nblk);
    attributes = (rdz_attribute *)(void *)(objects + nobj);

    offset = RDZ_DIRECTORY_HEADER_LEN;
    for (i = 0; i < nobj; i++, offset += RDZ_OBJECT_ENTRY_LEN) {
        rdz_object_decode(d + offset, &objects[i]);
    }
    for (i = 0; i < natt; i++, offset += RDZ_ATTRIBUTE_ENTRY_LEN) {
        if (rdz_attribute_decode(d + offset, &attributes[i])) {
            return rdz_invalid(e, "nonzero attribute directory reserved field");
        }
    }
    for (i = 0; i < nblk; i++, offset += RDZ_BLOCK_ENTRY_LEN) {
        rdz_block *b = &blocks[i];
        uint64_t payload;
        if (rdz_block_entry_decode(d + offset, b)) {
            return rdz_invalid(e, "nonzero block directory reserved field");
        }
        if (b->sequence != i || b->flags != 0) {
            return rdz_invalid(e, "invalid block sequence or flags");
        }
        if (b->decoded_len > RDZ_MAX_BLOCK_SIZE) return rdz_limit(e, "block size");
        switch (b->compression) {
        case RDZ_COMPRESSION_NONE:
            if ((uint64_t)b->stored_len != b->decoded_len) return rdz_limit(e, "block size");
            break;
        case RDZ_COMPRESSION_ZSTD:
            /* canonical: a block is compressed only when that makes it smaller */
            if (b->stored_len == 0 || (uint64_t)b->stored_len >= b->decoded_len) {
                return rdz_invalid(e, "a compressed block is not smaller than its decoded bytes");
            }
            break;
        default:
            return rdz_invalid(e, "unsupported block compression");
        }
        if (b->decoded_len > r->block_size) {
            return rdz_invalid(e, "decoded block length exceeds the declared block size");
        }
        if (rdz_add_u64(b->header_offset, RDZ_BLOCK_HEADER_LEN, &payload, e)) return 1;
        if (b->header_offset != next_block || b->payload_offset != payload) {
            return rdz_invalid(e, "non-canonical block offsets");
        }
        if (rdz_add_u64(b->payload_offset, b->stored_len, &next_block, e)) return 1;
        if (next_block > r->directory_offset) {
            return rdz_invalid(e, "block overlaps the directory");
        }
    }
    if (next_block != r->directory_offset) {
        return rdz_invalid(e, "gap or trailing bytes before the directory");
    }
    r->nobjects = nobj;
    r->nattributes = natt;
    r->nblocks = nblk;
    r->objects = objects;
    r->attributes = attributes;
    r->blocks = blocks;
    r->synopsis = d + offset;
    r->synopsis_len = (uint32_t)synopsis_len;
    switch (r->codec_id) {
    case RDZ_CODEC_R_SERIAL_V3:
        return rdz_check_generic_schema(r, e);
    case RDZ_CODEC_NATIVE_V1:
        return rdz_check_native_logical_schema(r, e);
    default:
        return rdz_invalid(e, "unsupported codec directory");
    }
}

static int rdz_reader_validate(rdz_reader *r, rdz_error *e)
{
    uint8_t header[RDZ_HEADER_LEN], trailer[RDZ_TRAILER_LEN];
    uint64_t file_len = r->file.size, trailer_offset, end, directory_len, checksum;
    const uint64_t max_directory =
        RDZ_DIRECTORY_HEADER_LEN + (uint64_t)RDZ_MAX_OBJECTS * RDZ_OBJECT_ENTRY_LEN +
        (uint64_t)RDZ_MAX_ATTRIBUTES * RDZ_ATTRIBUTE_ENTRY_LEN +
        (uint64_t)RDZ_MAX_BLOCKS * RDZ_BLOCK_ENTRY_LEN + RDZ_MAX_SYNOPSIS_LEN;
    uint32_t i;

    if (file_len < RDZ_HEADER_LEN + RDZ_BLOCK_HEADER_LEN + RDZ_DIRECTORY_HEADER_LEN + RDZ_TRAILER_LEN) {
        return rdz_invalid(e, "file is truncated");
    }
    if (rdz_infile_read_at(&r->file, 0, header, sizeof header, e)) return 1;
    if (memcmp(header, RDZ_FILE_MAGIC, 4) != 0) return rdz_invalid(e, "incorrect magic bytes");
    r->container_version = zb_rd_u16le(header + RDZ_FH_VERSION);
    if (r->container_version != RDZ_CONTAINER_VERSION) {
        return rdz_version_error(e, r->container_version);
    }
    if (zb_rd_u16le(header + RDZ_FH_HEADER_LEN) != RDZ_HEADER_LEN) {
        return rdz_invalid(e, "unsupported header length");
    }
    if (zb_rd_u32le(header + RDZ_FH_FLAGS) != 0 || zb_rd_u32le(header + RDZ_FH_RESERVED) != 0) {
        return rdz_invalid(e, "unsupported header flags or reserved fields");
    }
    if (RDZ_CHECKSUM_DIFFERS(zb_rd_u64le(header + RDZ_FH_CHECKSUM),
                             rdz_hash(header, RDZ_FH_CHECKSUM))) {
        return rdz_invalid(e, "header checksum mismatch");
    }
    r->codec_id = zb_rd_u16le(header + RDZ_FH_CODEC);
    r->codec_version = zb_rd_u16le(header + RDZ_FH_CODEC_VERSION);
    if (rdz_check_codec(r->codec_id, r->codec_version, e)) return 1;
    r->block_size = zb_rd_u32le(header + RDZ_FH_MAX_BLOCK);
    if (r->block_size == 0 || r->block_size > RDZ_MAX_BLOCK_SIZE) {
        return rdz_limit(e, "maximum decoded block size");
    }

    trailer_offset = file_len - RDZ_TRAILER_LEN;
    if (rdz_infile_read_at(&r->file, trailer_offset, trailer, sizeof trailer, e)) return 1;
    if (memcmp(trailer + RDZ_TR_MAGIC, RDZ_TRAILER_MAGIC, 4) != 0 ||
        memcmp(trailer + RDZ_TR_CLOSING, RDZ_CLOSING_MAGIC, 4) != 0) {
        return rdz_invalid(e, "closing trailer magic mismatch");
    }
    if (zb_rd_u16le(trailer + RDZ_TR_VERSION) != RDZ_CONTAINER_VERSION ||
        zb_rd_u16le(trailer + RDZ_TR_TRAILER_LEN) != RDZ_TRAILER_LEN ||
        zb_rd_u32le(trailer + RDZ_TR_RESERVED) != 0) {
        return rdz_invalid(e, "unsupported closing trailer");
    }
    r->directory_offset = zb_rd_u64le(trailer + RDZ_TR_DIRECTORY_OFFSET);
    directory_len = zb_rd_u64le(trailer + RDZ_TR_DIRECTORY_LEN);
    checksum = zb_rd_u64le(trailer + RDZ_TR_CHECKSUM);
    if (r->directory_offset < RDZ_HEADER_LEN) {
        return rdz_invalid(e, "directory overlaps the file header");
    }
    if (rdz_add_u64(r->directory_offset, directory_len, &end, e)) return 1;
    if (end != trailer_offset) {
        return rdz_invalid(e, "directory bounds do not reach the closing trailer");
    }
    if (directory_len > max_directory) return rdz_limit(e, "directory length");
    if (zb_buf_alloc(&r->directory, (size_t)directory_len, 0) ||
        !zb_put_raw(&r->directory, (size_t)directory_len)) {
        return rdz_memory(e, "the directory");
    }
    if (rdz_infile_read_at(&r->file, r->directory_offset, r->directory.data,
                           r->directory.len, e)) {
        return 1;
    }
    if (RDZ_CHECKSUM_DIFFERS(checksum, rdz_hash(r->directory.data, r->directory.len))) {
        return rdz_invalid(e, "directory checksum mismatch");
    }
    if (rdz_parse_directory(r, e)) return 1;
    r->payload_bytes = 0;
    for (i = 0; i < r->nblocks; i++) {
        if (rdz_add_u64(r->payload_bytes, r->blocks[i].decoded_len, &r->payload_bytes, e)) return 1;
    }
    r->file_bytes = file_len;
    return 0;
}

int rdz_reader_open(rdz_reader *r, const char *path, rdz_error *e)
{
    rdz_reader_init(r);
    if (rdz_infile_open(&r->file, path, e) || rdz_reader_validate(r, e)) {
        rdz_reader_close(r);
        return 1;
    }
    return 0;
}

int rdz_reader_open_memory(rdz_reader *r, const uint8_t *data, size_t n, rdz_error *e)
{
    rdz_reader_init(r);
    rdz_infile_open_memory(&r->file, data, n);
    if (rdz_reader_validate(r, e)) {
        rdz_reader_close(r);
        return 1;
    }
    return 0;
}

int rdz_reader_read_stored(rdz_reader *r, uint32_t index, zb_buf *stored, rdz_error *e)
{
    const rdz_block *b;
    uint8_t h[RDZ_BLOCK_HEADER_LEN];
    uint64_t end;
    if (index >= r->nblocks) return rdz_invalid(e, "block index is out of range");
    b = &r->blocks[index];
    if (rdz_add_u64(b->payload_offset, b->stored_len, &end, e)) return 1;
    if (end > r->directory_offset) return rdz_invalid(e, "block payload exceeds its bounds");
    if (rdz_infile_read_at(&r->file, b->header_offset, h, sizeof h, e)) return 1;
    if (memcmp(h, RDZ_BLOCK_MAGIC, 4) != 0 ||
        zb_rd_u16le(h + RDZ_BH_HEADER_LEN) != RDZ_BLOCK_HEADER_LEN ||
        zb_rd_u16le(h + RDZ_BH_FLAGS) != 0 || zb_rd_u32le(h + RDZ_BH_SEQUENCE) != b->sequence ||
        zb_rd_u16le(h + RDZ_BH_ENCODING) != b->encoding ||
        zb_rd_u16le(h + RDZ_BH_COMPRESSION) != b->compression ||
        zb_rd_u64le(h + RDZ_BH_LOGICAL_COUNT) != b->logical_count ||
        zb_rd_u64le(h + RDZ_BH_DECODED_LEN) != b->decoded_len ||
        zb_rd_u32le(h + RDZ_BH_STORED_LEN) != b->stored_len ||
        zb_rd_u32le(h + RDZ_BH_RESERVED) != 0 || zb_rd_u64le(h + RDZ_BH_CHECKSUM) != b->checksum) {
        return rdz_invalid_block(e, "block %lu header does not match the directory", b->sequence);
    }
    zb_buf_reset(stored);
    if (!zb_put_raw(stored, b->stored_len)) {
        return (stored->flags & ZB_BUF_HIT_LIMIT) ? rdz_limit(e, "block allocation")
                                                  : rdz_memory(e, "a block");
    }
    return rdz_infile_read_at(&r->file, b->payload_offset, stored->data, b->stored_len, e);
}

int rdz_block_decode(const rdz_block *b, const zb_buf *stored, zb_buf *out, rdz_codec *codec,
                     rdz_error *e)
{
    if (stored->len != b->stored_len) return rdz_invalid(e, "block length mismatch");
    if (RDZ_CHECKSUM_DIFFERS(b->checksum, rdz_hash(stored->data, stored->len))) {
        return rdz_invalid_block(e, "checksum mismatch in block %lu", b->sequence);
    }
    if (b->compression == RDZ_COMPRESSION_NONE) return 0;
    zb_buf_reset(out);
    if (!zb_put_raw(out, (size_t)b->decoded_len)) {
        return (out->flags & ZB_BUF_HIT_LIMIT) ? rdz_limit(e, "block allocation")
                                               : rdz_memory(e, "a block");
    }
    return rdz_codec_decompress(codec, b->compression, stored->data, stored->len, out->data,
                                (size_t)b->decoded_len, b->sequence, e);
}

int rdz_reader_read_block(rdz_reader *r, uint32_t index, zb_buf *out, rdz_error *e)
{
    const rdz_block *b;
    if (rdz_reader_read_stored(r, index, &r->scratch, e)) return 1;
    b = &r->blocks[index];
    if (rdz_block_decode(b, &r->scratch, out, &r->codec, e)) return 1;
    if (b->compression == RDZ_COMPRESSION_NONE) {
        zb_buf_reset(out);
        if (!zb_put_raw(out, r->scratch.len)) {
            return (out->flags & ZB_BUF_HIT_LIMIT) ? rdz_limit(e, "block allocation")
                                                   : rdz_memory(e, "a block");
        }
        if (r->scratch.len) memcpy(out->data, r->scratch.data, r->scratch.len);
    }
    return 0;
}

/* ---- writer ------------------------------------------------------------------------ */

void rdz_writer_init(rdz_writer *w)
{
    rdz_outfile_init(&w->out);
    zb_buf_init(&w->entries);
    w->nblocks = 0;
    w->block_size = RDZ_BLOCK_SIZE;
    w->open = 0;
}

void rdz_writer_discard(rdz_writer *w)
{
    rdz_outfile_discard(&w->out);
    zb_buf_release(&w->entries);
    w->nblocks = 0;
    w->open = 0;
}

int rdz_writer_open(rdz_writer *w, const char *path, uint16_t codec_id,
                    uint16_t codec_version, uint32_t block_size, rdz_error *e)
{
    uint8_t h[RDZ_HEADER_LEN];
    rdz_writer_init(w);
    if (block_size == 0 || block_size > RDZ_MAX_BLOCK_SIZE) return rdz_limit(e, "block size");
    w->block_size = block_size;
    if (zb_buf_alloc(&w->entries, RDZ_BLOCK_ENTRY_LEN * 4, 0)) {
        return rdz_memory(e, "the block directory");
    }
    if (rdz_outfile_open(&w->out, path, e)) {
        rdz_writer_discard(w);
        return 1;
    }
    w->open = 1;
    memset(h, 0, sizeof h);
    memcpy(h + RDZ_FH_MAGIC, RDZ_FILE_MAGIC, 4);
    zb_wr_u16le(h + RDZ_FH_VERSION, (uint16_t)RDZ_CONTAINER_VERSION);
    zb_wr_u16le(h + RDZ_FH_HEADER_LEN, (uint16_t)RDZ_HEADER_LEN);
    zb_wr_u16le(h + RDZ_FH_CODEC, codec_id);
    zb_wr_u16le(h + RDZ_FH_CODEC_VERSION, codec_version);
    zb_wr_u32le(h + RDZ_FH_MAX_BLOCK, block_size);
    zb_wr_u64le(h + RDZ_FH_CHECKSUM, rdz_hash(h, RDZ_FH_CHECKSUM));
    return rdz_outfile_write(&w->out, h, sizeof h, e);
}

int rdz_writer_stored(rdz_writer *w, uint16_t encoding, uint16_t compression,
                      uint64_t logical_count, uint64_t decoded_len, const uint8_t *stored,
                      size_t n, uint64_t checksum, rdz_error *e)
{
    uint8_t h[RDZ_BLOCK_HEADER_LEN];
    uint8_t *slot;
    rdz_block b;
    if (w->nblocks >= RDZ_MAX_BLOCKS) return rdz_limit(e, "block count");
    if ((uint64_t)n > RDZ_MAX_BLOCK_SIZE || decoded_len > w->block_size) {
        return rdz_limit(e, "block size");
    }
    b.sequence = w->nblocks;
    b.flags = 0;
    b.header_offset = w->out.position;
    if (rdz_add_u64(b.header_offset, RDZ_BLOCK_HEADER_LEN, &b.payload_offset, e)) return 1;
    b.stored_len = (uint32_t)n;
    b.logical_count = logical_count;
    b.decoded_len = decoded_len;
    b.encoding = encoding;
    b.compression = compression;
    b.checksum = checksum;
    slot = zb_put_raw(&w->entries, RDZ_BLOCK_ENTRY_LEN);
    if (!slot) return rdz_memory(e, "the block directory");
    rdz_block_entry_encode(slot, &b);
    rdz_block_header_encode(h, &b);
    if (rdz_outfile_write(&w->out, h, sizeof h, e) || rdz_outfile_write(&w->out, stored, n, e)) {
        return 1;
    }
    w->nblocks++;
    return 0;
}

int rdz_writer_block(rdz_writer *w, uint16_t encoding, uint64_t logical_count,
                     const uint8_t *payload, size_t n, rdz_error *e)
{
    return rdz_writer_stored(w, encoding, RDZ_COMPRESSION_NONE, logical_count, (uint64_t)n,
                             payload, n, rdz_hash(payload, n), e);
}

int rdz_writer_finish(rdz_writer *w, const rdz_object *objects, uint32_t nobjects,
                      const rdz_attribute *attributes, uint32_t nattributes,
                      const uint8_t *synopsis, size_t synopsis_len, rdz_error *e)
{
    zb_buf dir;
    uint8_t t[RDZ_TRAILER_LEN];
    uint8_t *p;
    uint64_t directory_offset = w->out.position;
    size_t len;
    uint32_t i;
    int failed;

    if (synopsis_len > RDZ_MAX_SYNOPSIS_LEN) return rdz_limit(e, "generic synopsis");
    if (nobjects > RDZ_MAX_OBJECTS) return rdz_limit(e, "object count");
    if (nattributes > RDZ_MAX_ATTRIBUTES) return rdz_limit(e, "attribute count");
    len = RDZ_DIRECTORY_HEADER_LEN + (size_t)nobjects * RDZ_OBJECT_ENTRY_LEN +
          (size_t)nattributes * RDZ_ATTRIBUTE_ENTRY_LEN + w->entries.len + synopsis_len;
    if (zb_buf_alloc(&dir, len, 0) || !(p = zb_put_raw(&dir, len))) {
        zb_buf_release(&dir);
        return rdz_memory(e, "the directory");
    }
    memset(p, 0, RDZ_DIRECTORY_HEADER_LEN);
    memcpy(p + RDZ_DH_MAGIC, RDZ_DIRECTORY_MAGIC, 4);
    zb_wr_u16le(p + RDZ_DH_VERSION, (uint16_t)RDZ_DIRECTORY_VERSION);
    zb_wr_u16le(p + RDZ_DH_HEADER_LEN, (uint16_t)RDZ_DIRECTORY_HEADER_LEN);
    zb_wr_u16le(p + RDZ_DH_OBJECT_WIDTH, (uint16_t)RDZ_OBJECT_ENTRY_LEN);
    zb_wr_u16le(p + RDZ_DH_ATTRIBUTE_WIDTH, (uint16_t)RDZ_ATTRIBUTE_ENTRY_LEN);
    zb_wr_u16le(p + RDZ_DH_BLOCK_WIDTH, (uint16_t)RDZ_BLOCK_ENTRY_LEN);
    zb_wr_u32le(p + RDZ_DH_OBJECTS, nobjects);
    zb_wr_u32le(p + RDZ_DH_ATTRIBUTES, nattributes);
    zb_wr_u32le(p + RDZ_DH_BLOCKS, w->nblocks);
    zb_wr_u32le(p + RDZ_DH_SYNOPSIS_LEN, (uint32_t)synopsis_len);
    zb_wr_u64le(p + RDZ_DH_CHECKSUM, rdz_hash(p, RDZ_DH_CHECKSUM));
    p += RDZ_DIRECTORY_HEADER_LEN;
    for (i = 0; i < nobjects; i++, p += RDZ_OBJECT_ENTRY_LEN) rdz_object_encode(p, &objects[i]);
    for (i = 0; i < nattributes; i++, p += RDZ_ATTRIBUTE_ENTRY_LEN) {
        rdz_attribute_encode(p, &attributes[i]);
    }
    if (w->entries.len) memcpy(p, w->entries.data, w->entries.len);
    p += w->entries.len;
    if (synopsis_len) memcpy(p, synopsis, synopsis_len);

    memset(t, 0, sizeof t);
    memcpy(t + RDZ_TR_MAGIC, RDZ_TRAILER_MAGIC, 4);
    zb_wr_u16le(t + RDZ_TR_VERSION, (uint16_t)RDZ_CONTAINER_VERSION);
    zb_wr_u16le(t + RDZ_TR_TRAILER_LEN, (uint16_t)RDZ_TRAILER_LEN);
    zb_wr_u64le(t + RDZ_TR_DIRECTORY_OFFSET, directory_offset);
    zb_wr_u64le(t + RDZ_TR_DIRECTORY_LEN, (uint64_t)len);
    zb_wr_u64le(t + RDZ_TR_CHECKSUM, rdz_hash(dir.data, len));
    memcpy(t + RDZ_TR_CLOSING, RDZ_CLOSING_MAGIC, 4);

    failed = rdz_outfile_write(&w->out, dir.data, len, e) ||
             rdz_outfile_write(&w->out, t, sizeof t, e) || rdz_outfile_commit(&w->out, e);
    zb_buf_release(&dir);
    if (failed) return 1;
    rdz_writer_discard(w);
    return 0;
}

int rdz_write_generic(const char *path, const uint8_t *payload, size_t n,
                      const uint8_t *synopsis, size_t synopsis_len, rdz_error *e)
{
    rdz_writer w;
    size_t at = 0;
    if (synopsis_len > RDZ_MAX_SYNOPSIS_LEN) return rdz_limit(e, "generic synopsis");
    if (n / RDZ_BLOCK_SIZE >= RDZ_MAX_BLOCKS) return rdz_limit(e, "block count");
    if (rdz_writer_open(&w, path, RDZ_CODEC_R_SERIAL_V3, RDZ_R_SERIAL_CODEC_VERSION,
                        RDZ_BLOCK_SIZE, e)) {
        rdz_writer_discard(&w);
        return 1;
    }
    do {
        size_t chunk = n - at < RDZ_BLOCK_SIZE ? n - at : RDZ_BLOCK_SIZE;
        if (rdz_writer_block(&w, RDZ_ENCODING_RAW, chunk, payload + at, chunk, e)) {
            rdz_writer_discard(&w);
            return 1;
        }
        at += chunk;
    } while (at < n);
    if (rdz_writer_finish(&w, NULL, 0, NULL, 0, synopsis, synopsis_len, e)) {
        rdz_writer_discard(&w);
        return 1;
    }
    return 0;
}
