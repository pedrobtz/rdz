#include <stdlib.h>
#include <string.h>

#include "rdz_logical.h"
#include "rdz_numeric.h"
#include "rdz_graph.h"
#include "rdz_vector.h"

size_t rdz_vec_block_values(uint16_t type)
{
    switch (type) {
    case RDZ_TYPE_INTEGER: return RDZ_INT_BLOCK_VALUES;
    case RDZ_TYPE_DOUBLE: return RDZ_DBL_BLOCK_VALUES;
    default: return RDZ_LOGICAL_BLOCK_VALUES;
    }
}

void rdz_vec_init(rdz_vec *v)
{
    rdz_writer_init(&v->w);
    memset(&v->pipe, 0, sizeof v->pipe);
    v->have_pipe = 0;
    v->hash_content = 0;
    v->content = NULL;
    v->content_mem = NULL;
    v->metadata = NULL;
    v->metadata_len = 0;
    zb_buf_alloc(&v->block_plan, 0, 0); /* empty: allocating nothing, cannot fail */
    zb_buf_alloc(&v->scratch, 0, 0);
}

void rdz_vec_free(rdz_vec *v)
{
    free(v->content_mem);
    v->content_mem = NULL;
    zb_buf_release(&v->w.result);
    zb_buf_release(&v->block_plan);
    zb_buf_release(&v->scratch);
    v->content = NULL;
    if (v->have_pipe) rdz_pipeline_free(&v->pipe); /* joins the workers first */
    v->have_pipe = 0;
    rdz_writer_discard(&v->w);
}

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

int rdz_vec_write(rdz_vec *v, const char *path, const rdz_vec_spec *spec, rdz_error *e)
{
    rdz_node nodes[4];
    rdz_attribute attribute;
    rdz_str_source one;
    uint32_t k = 1;
    int factor = spec->type == RDZ_TYPE_FACTOR;
    memset(nodes, 0, sizeof nodes);
    memset(&attribute, 0, sizeof attribute);
    if (spec->names && spec->names->n != spec->n) {
        return rdz_invalid(e, "names length does not match the vector");
    }
    if (factor && spec->names) return rdz_invalid(e, "a native factor has no names");
    nodes[0].type = spec->type;
    nodes[0].role = RDZ_ROLE_ROOT;
    nodes[0].parent = RDZ_ROOT_PARENT_ID;
    nodes[0].flags = factor && spec->ordered ? RDZ_OBJECT_FLAG_ORDERED : 0;
    nodes[0].length = spec->n;
    nodes[0].values = spec->values;
    nodes[0].strings = spec->strings;
    if (factor) {
        nodes[0].first_child = 1;
        nodes[0].child_count = 1;
        nodes[1].type = RDZ_TYPE_CHARACTER;
        nodes[1].role = RDZ_ROLE_LEVELS;
        nodes[1].length = spec->levels->n;
        nodes[1].strings = spec->levels;
        k = 2;
    }
    if (spec->names) {
        one.n = 1;
        one.ctx = NULL;
        one.key = one_key;
        one.value = one_value;
        nodes[0].attribute_count = 1;
        nodes[k].type = RDZ_TYPE_CHARACTER;
        nodes[k].role = RDZ_ROLE_ATTRIBUTE_NAME;
        nodes[k].length = 1;
        nodes[k].strings = &one;
        nodes[k + 1].type = RDZ_TYPE_CHARACTER;
        nodes[k + 1].role = RDZ_ROLE_ATTRIBUTE_VALUE;
        nodes[k + 1].length = spec->n;
        nodes[k + 1].strings = spec->names;
        attribute.name_object_id = k;
        attribute.value_object_id = k + 1;
        attribute.flags = RDZ_ATTRIBUTE_FLAG_NAMES;
        k += 2;
    }
    return rdz_graph_write(v, path, nodes, k, &attribute, spec->names ? 1 : 0, spec->policy,
                           spec->level, spec->threads, spec->tick, spec->tick_ctx, e);
}

int rdz_vec_shape(const rdz_reader *r, uint16_t *type, size_t *n, rdz_error *e)
{
    if (r->codec_id != RDZ_CODEC_NATIVE_V1 || r->nobjects == 0) {
        return rdz_codec_error(e, r->codec_id, r->codec_version);
    }
    *type = r->objects[0].type_tag;
    if (r->objects[0].logical_len > (uint64_t)SIZE_MAX / sizeof(double)) {
        return rdz_limit(e, "allocation size");
    }
    *n = (size_t)r->objects[0].logical_len;
    return 0;
}

static int decode_into(uint16_t type, const rdz_block *b, const zb_buf *rec, void *out,
                       size_t at, rdz_error *e)
{
    size_t n = (size_t)b->logical_count;
    switch (type) {
    case RDZ_TYPE_LOGICAL:
        return rdz_logical_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at, e);
    case RDZ_TYPE_INTEGER:
    case RDZ_TYPE_FACTOR:
        return rdz_int_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at, e);
    default:
        return rdz_dbl_decode(rec->data, rec->len, b->encoding, n, (double *)out + at, e);
    }
}

/* Decodes `count` records from a verified block into r->records. */
static int decode_records_into(rdz_reader *r, const zb_buf *rec, size_t count, rdz_error *e)
{
    size_t bytes;
    if (zb_size_mul(count, sizeof(rdz_str), &bytes)) return rdz_limit(e, "character values");
    zb_buf_reset(&r->records);
    if (!zb_put_raw(&r->records, bytes)) return rdz_memory(e, "character values");
    return rdz_string_decode_records(rec->data, rec->len, count,
                                     (rdz_str *)(void *)r->records.data, e);
}

int rdz_vec_read_strings(rdz_vec *v, rdz_reader *r, uint32_t object, const rdz_names_sink *sink,
                         int threads, rdz_tick_fn tick, void *tick_ctx, rdz_error *e)
{
    const rdz_object *o = &r->objects[object];
    uint32_t next = o->first_block, end = o->first_block + o->block_count;
    size_t elements = 0, entries = 0;
    if (v->have_pipe) {
        rdz_pipeline_free(&v->pipe);
        v->have_pipe = 0;
    }
    if (rdz_pipeline_init(&v->pipe, o->block_count > 1 ? threads : 1, rdz_job_decode,
                          (size_t)RDZ_MAX_BLOCK_SIZE, e)) {
        return 1;
    }
    v->have_pipe = 1;
    for (;;) {
        rdz_slot *s;
        const rdz_block *b;
        size_t count;
        while (next < end && v->pipe.next_submit - v->pipe.next_consume < v->pipe.nslots) {
            int must;
            s = rdz_pipeline_next(&v->pipe, &must);
            if (rdz_reader_read_stored(r, next, &s->in, e)) {
                rdz_pipeline_unget(&v->pipe, s);
                return 1;
            }
            s->block = &r->blocks[next++];
            rdz_pipeline_submit(&v->pipe, s, e);
        }
        s = rdz_pipeline_oldest(&v->pipe, 1);
        if (!s) break;
        if (s->failed) {
            *e = s->e;
            return 1;
        }
        b = (const rdz_block *)s->block;
        count = (size_t)b->logical_count;
        switch (b->encoding) {
        case RDZ_ENCODING_STRING_PLAIN:
        case RDZ_ENCODING_STRING_DICT_ENTRIES:
            if (decode_records_into(r, s->result, count, e)) return 1;
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
            if (rdz_string_decode_indices(s->result->data, s->result->len, count, entries,
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
        rdz_pipeline_release(&v->pipe, s);
        if (tick && (next < end || v->pipe.next_consume != v->pipe.next_submit)) tick(tick_ctx);
    }
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;
    if (elements != o->logical_len) return rdz_invalid(e, "character object length mismatch");
    return 0;
}

int rdz_vec_read(rdz_vec *v, rdz_reader *r, void *out, int threads, rdz_tick_fn tick,
                 void *tick_ctx, rdz_error *e)
{
    const rdz_object *root;
    uint16_t type;
    size_t n, at = 0;
    uint32_t next, end;
    if (rdz_vec_shape(r, &type, &n, e)) return 1;
    if (type == RDZ_TYPE_CHARACTER) return rdz_invalid(e, "a character root is read as strings");
    root = &r->objects[0];
    next = root->first_block;
    end = root->first_block + root->block_count;
    if (v->have_pipe) {
        rdz_pipeline_free(&v->pipe);
        v->have_pipe = 0;
    }
    if (rdz_pipeline_init(&v->pipe, root->block_count > 1 ? threads : 1, rdz_job_decode,
                          (size_t)RDZ_MAX_BLOCK_SIZE, e)) {
        return 1;
    }
    v->have_pipe = 1;
    for (;;) {
        rdz_slot *s;
        /* read stored bytes ahead into every free slot (file IO here only) */
        while (next < end && v->pipe.next_submit - v->pipe.next_consume < v->pipe.nslots) {
            int must;
            s = rdz_pipeline_next(&v->pipe, &must);
            if (rdz_reader_read_stored(r, next, &s->in, e)) {
                rdz_pipeline_unget(&v->pipe, s);
                return 1;
            }
            s->block = &r->blocks[next++];
            rdz_pipeline_submit(&v->pipe, s, e);
        }
        s = rdz_pipeline_oldest(&v->pipe, 1);
        if (!s) break;
        if (s->failed) {
            *e = s->e;
            return 1;
        } else {
            const rdz_block *b = (const rdz_block *)s->block;
            if (b->logical_count > n - at) return rdz_invalid(e, "native payload length mismatch");
            if (decode_into(type, b, s->result, out, at, e)) return 1;
            at += (size_t)b->logical_count;
        }
        rdz_pipeline_release(&v->pipe, s);
        if (tick && (next < end || v->pipe.next_consume != v->pipe.next_submit)) tick(tick_ctx);
    }
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;
    if (at != n) return rdz_invalid(e, "native payload length mismatch");
    return 0;
}
