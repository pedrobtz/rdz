#include <string.h>

#include "rdz_logical.h"
#include "rdz_native.h"

/* ---- writing ---------------------------------------------------------------------- */

static int emit_block(void *ctx, uint16_t encoding, uint64_t count, const uint8_t *data, size_t n,
                      rdz_error *e)
{
    return rdz_writer_block((rdz_writer *)ctx, encoding, count, data, n, e);
}

/* The attribute name "names" as a one-element source. */
static uintptr_t one_key(void *ctx, size_t i)
{
    (void)ctx;
    return (uintptr_t)i + 1;
}

static int one_value(void *ctx, size_t i, rdz_str *out, rdz_error *e)
{
    (void)ctx;
    (void)i;
    (void)e;
    out->tag = RDZ_STR_NATIVE;
    out->bytes = (const uint8_t *)"names";
    out->len = 5;
    return 0;
}

int rdz_write_native_logical(const char *path, const int32_t *values, size_t n,
                             const rdz_str_source *names, int policy, rdz_error *e)
{
    rdz_writer w;
    zb_buf block, scratch;
    rdz_object objects[3];
    rdz_attribute attribute;
    uint32_t lblocks, name_first, value_first;
    size_t at = 0;
    int failed = 1;

    memset(&attribute, 0, sizeof attribute);
    if (names && names->n != n) return rdz_invalid(e, "names length does not match logical length");
    zb_buf_alloc(&block, 0, 0);
    zb_buf_alloc(&scratch, 0, 0);
    if (rdz_writer_open(&w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION, RDZ_BLOCK_SIZE,
                        e)) {
        goto done;
    }
    do {
        size_t take = n - at < RDZ_LOGICAL_BLOCK_VALUES ? n - at : RDZ_LOGICAL_BLOCK_VALUES;
        uint16_t encoding;
        if (rdz_logical_encode(values + at, take, &block, &encoding, &scratch, e) ||
            rdz_writer_block(&w, encoding, take, block.data, block.len, e)) {
            goto done;
        }
        at += take;
    } while (at < n);
    lblocks = w.nblocks;

    memset(objects, 0, sizeof objects);
    objects[0].object_id = 0;
    objects[0].parent_id = RDZ_ROOT_PARENT_ID;
    objects[0].role = RDZ_ROLE_ROOT;
    objects[0].type_tag = RDZ_TYPE_LOGICAL;
    objects[0].logical_len = n;
    objects[0].attribute_count = names ? 1 : 0;
    objects[0].first_block = 0;
    objects[0].block_count = lblocks;

    if (names) {
        rdz_str_source one;
        one.n = 1;
        one.ctx = NULL;
        one.key = one_key;
        one.value = one_value;
        name_first = w.nblocks;
        if (rdz_string_encode(&one, RDZ_DICT_PLAIN, emit_block, &w, e)) goto done;
        value_first = w.nblocks;
        if (rdz_string_encode(names, policy, emit_block, &w, e)) goto done;

        objects[1].object_id = 1;
        objects[1].parent_id = 0;
        objects[1].role = RDZ_ROLE_ATTRIBUTE_NAME;
        objects[1].type_tag = RDZ_TYPE_CHARACTER;
        objects[1].logical_len = 1;
        objects[1].first_block = name_first;
        objects[1].block_count = value_first - name_first;
        objects[2].object_id = 2;
        objects[2].parent_id = 0;
        objects[2].role = RDZ_ROLE_ATTRIBUTE_VALUE;
        objects[2].type_tag = RDZ_TYPE_CHARACTER;
        objects[2].logical_len = n;
        objects[2].first_block = value_first;
        objects[2].block_count = w.nblocks - value_first;
        attribute.owner_id = 0;
        attribute.name_object_id = 1;
        attribute.value_object_id = 2;
        attribute.ordinal = 0;
        attribute.flags = RDZ_ATTRIBUTE_FLAG_NAMES;
    }
    if (rdz_writer_finish(&w, objects, names ? 3 : 1, &attribute, names ? 1 : 0, NULL, 0, e)) {
        goto done;
    }
    failed = 0;
done:
    rdz_writer_discard(&w);
    zb_buf_release(&block);
    zb_buf_release(&scratch);
    return failed;
}

/* ---- reading ---------------------------------------------------------------------- */

int rdz_native_length(const rdz_reader *r, size_t *n, rdz_error *e)
{
    if (r->codec_id != RDZ_CODEC_NATIVE_V1 || r->nobjects == 0) {
        return rdz_codec_error(e, r->codec_id, r->codec_version);
    }
    if (r->objects[0].logical_len > (uint64_t)SIZE_MAX / sizeof(int32_t)) {
        return rdz_limit(e, "allocation size");
    }
    *n = (size_t)r->objects[0].logical_len;
    return 0;
}

int rdz_native_read_logical(rdz_reader *r, int32_t *out, rdz_error *e)
{
    const rdz_object *root;
    uint32_t i;
    size_t at = 0, n;
    if (rdz_native_length(r, &n, e)) return 1;
    root = &r->objects[0];
    for (i = root->first_block; i < root->first_block + root->block_count; i++) {
        const rdz_block *b = &r->blocks[i];
        if (rdz_reader_read_block(r, i, &r->decoded, e)) return 1;
        if (b->logical_count > n - at) return rdz_invalid(e, "logical payload length mismatch");
        if (rdz_logical_decode(r->decoded.data, r->decoded.len, b->encoding,
                               (size_t)b->logical_count, out + at, e)) {
            return 1;
        }
        at += (size_t)b->logical_count;
    }
    if (at != n) return rdz_invalid(e, "logical payload length mismatch");
    return 0;
}

int rdz_native_names_info(const rdz_reader *r, int *present, size_t *length,
                          size_t *dictionary_length, rdz_error *e)
{
    const rdz_object *value;
    uint32_t i;
    uint64_t entries = 0;
    *present = 0;
    *length = 0;
    *dictionary_length = 0;
    if (r->codec_id != RDZ_CODEC_NATIVE_V1) return rdz_codec_error(e, r->codec_id, r->codec_version);
    if (r->nattributes == 0) return 0;
    /* the validated shape: objects 1 and 2 are the name and its value */
    value = &r->objects[r->attributes[0].value_object_id];
    for (i = value->first_block; i < value->first_block + value->block_count; i++) {
        if (r->blocks[i].encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) {
            entries += r->blocks[i].logical_count;
        }
    }
    *present = 1;
    *length = (size_t)value->logical_len;
    *dictionary_length = (size_t)entries; /* validation bounds it by the length */
    return 0;
}

/* Decodes `block`'s records into r->records. */
static int decode_records(rdz_reader *r, const rdz_block *b, rdz_error *e)
{
    size_t bytes;
    if (zb_size_mul((size_t)b->logical_count, sizeof(rdz_str), &bytes)) {
        return rdz_limit(e, "character values");
    }
    zb_buf_reset(&r->records);
    if (!zb_put_raw(&r->records, bytes)) return rdz_memory(e, "character values");
    return rdz_string_decode_records(r->decoded.data, r->decoded.len, (size_t)b->logical_count,
                                     (rdz_str *)(void *)r->records.data, e);
}

int rdz_native_read_names(rdz_reader *r, const rdz_names_sink *sink, rdz_error *e)
{
    const rdz_object *name, *value;
    const rdz_str *strs;
    uint32_t i;
    size_t elements = 0, entries = 0;
    if (r->codec_id != RDZ_CODEC_NATIVE_V1) return rdz_codec_error(e, r->codec_id, r->codec_version);
    if (r->nattributes == 0) return 0;
    name = &r->objects[r->attributes[0].name_object_id];
    value = &r->objects[r->attributes[0].value_object_id];

    /* the attribute's name must be exactly "names" */
    if (name->block_count != 1 || rdz_reader_read_block(r, name->first_block, &r->decoded, e) ||
        decode_records(r, &r->blocks[name->first_block], e)) {
        return name->block_count != 1 ? rdz_invalid(e, "native attribute name is not names") : 1;
    }
    strs = (const rdz_str *)(const void *)r->records.data;
    if (r->blocks[name->first_block].logical_count != 1 || strs[0].tag != RDZ_STR_NATIVE ||
        strs[0].len != 5 || memcmp(strs[0].bytes, "names", 5) != 0) {
        return rdz_invalid(e, "native attribute name is not names");
    }

    for (i = value->first_block; i < value->first_block + value->block_count; i++) {
        const rdz_block *b = &r->blocks[i];
        size_t count = (size_t)b->logical_count;
        if (rdz_reader_read_block(r, i, &r->decoded, e)) return 1;
        switch (b->encoding) {
        case RDZ_ENCODING_STRING_PLAIN:
        case RDZ_ENCODING_STRING_DICT_ENTRIES:
            if (decode_records(r, b, e)) return 1;
            if ((b->encoding == RDZ_ENCODING_STRING_PLAIN ? sink->plain : sink->entries)(
                    sink->ctx, (const rdz_str *)(const void *)r->records.data, count, e)) {
                return 1;
            }
            if (b->encoding == RDZ_ENCODING_STRING_PLAIN) elements += count;
            else entries += count;
            break;
        case RDZ_ENCODING_STRING_DICT_INDICES: {
            size_t bytes;
            if (zb_size_mul(count, sizeof(uint32_t), &bytes)) return rdz_limit(e, "dictionary indices");
            zb_buf_reset(&r->ids);
            if (!zb_put_raw(&r->ids, bytes)) return rdz_memory(e, "dictionary indices");
            if (rdz_string_decode_indices(r->decoded.data, r->decoded.len, count, entries,
                                          (uint32_t *)(void *)r->ids.data, e) ||
                sink->indices(sink->ctx, (const uint32_t *)(const void *)r->ids.data, count, e)) {
                return 1;
            }
            elements += count;
            break;
        }
        default:
            return rdz_invalid(e, "unexpected character block encoding");
        }
    }
    if (elements != value->logical_len) return rdz_invalid(e, "character object length mismatch");
    return 0;
}
