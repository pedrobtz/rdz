#include <string.h>

#include "rdz_logical.h"
#include "rdz_numeric.h"
#include "rdz_vector.h"

size_t rdz_vec_block_values(uint16_t type)
{
    switch (type) {
    case RDZ_TYPE_INTEGER: return RDZ_INT_BLOCK_VALUES;
    case RDZ_TYPE_DOUBLE: return RDZ_DBL_BLOCK_VALUES;
    default: return RDZ_LOGICAL_BLOCK_VALUES;
    }
}

static size_t elem_size(uint16_t type)
{
    return type == RDZ_TYPE_DOUBLE ? sizeof(double) : sizeof(int32_t);
}

void rdz_vec_init(rdz_vec *v)
{
    rdz_writer_init(&v->w);
    memset(&v->pipe, 0, sizeof v->pipe);
    v->have_pipe = 0;
}

void rdz_vec_free(rdz_vec *v)
{
    if (v->have_pipe) rdz_pipeline_free(&v->pipe); /* joins the workers first */
    v->have_pipe = 0;
    rdz_writer_discard(&v->w);
}

static int emit_block(void *ctx, uint16_t encoding, uint64_t count, const uint8_t *data, size_t n,
                      rdz_error *e)
{
    return rdz_writer_block((rdz_writer *)ctx, encoding, count, data, n, e);
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

/* Writes one finished slot and frees it. */
static int consume(rdz_vec *v, rdz_slot *s, rdz_error *e)
{
    int failed = 0;
    if (s->failed) {
        *e = s->e;
        failed = 1;
    } else if (rdz_writer_stored(&v->w, s->encoding, s->compression, s->logical_count,
                                 s->decoded_len, s->result->data, s->result->len, s->checksum,
                                 e)) {
        failed = 1;
    }
    rdz_pipeline_release(&v->pipe, s);
    return failed;
}

typedef struct {
    rdz_vec *v;
    const rdz_vec_spec *spec;
} rdz_emit_ctx;

/* A character block from the string encoder into the pipeline, which
   compresses and checksums it. */
static int emit_pipeline(void *ctx, uint16_t encoding, uint64_t count, const uint8_t *data,
                         size_t n, rdz_error *e)
{
    rdz_emit_ctx *c = (rdz_emit_ctx *)ctx;
    rdz_slot *s;
    int must;
    while ((s = rdz_pipeline_next(&c->v->pipe, &must)) != NULL && must) {
        if (consume(c->v, s, e)) return 1;
    }
    if (n && zb_put_bytes(&s->in, data, n)) {
        rdz_pipeline_unget(&c->v->pipe, s);
        return rdz_memory(e, "a block");
    }
    s->encoding = encoding;
    s->logical_count = count;
    rdz_pipeline_submit(&c->v->pipe, s, e);
    while ((s = rdz_pipeline_oldest(&c->v->pipe, 0)) != NULL) {
        if (consume(c->v, s, e)) return 1;
    }
    if (c->spec->tick) c->spec->tick(c->spec->tick_ctx);
    return 0;
}

/* Appends one character object's blocks, uncompressed. */
static int write_strings_plain(rdz_vec *v, const rdz_str_source *src, int policy, rdz_error *e)
{
    return rdz_string_encode(src, policy, emit_block, &v->w, e);
}

int rdz_vec_write(rdz_vec *v, const char *path, const rdz_vec_spec *spec, rdz_error *e)
{
    uint16_t type = spec->type, values_type = type == RDZ_TYPE_FACTOR ? RDZ_TYPE_INTEGER : type;
    size_t n = spec->n, per = rdz_vec_block_values(values_type), size = elem_size(values_type),
           at = 0;
    const uint8_t *src = (const uint8_t *)spec->values;
    rdz_object objects[4];
    rdz_attribute attribute;
    rdz_slot *s;
    uint32_t k = 1, first;
    int factor = type == RDZ_TYPE_FACTOR;

    memset(objects, 0, sizeof objects);
    memset(&attribute, 0, sizeof attribute);
    if (spec->names && spec->names->n != n) {
        return rdz_invalid(e, "names length does not match the vector");
    }
    if (factor && spec->names) return rdz_invalid(e, "a native factor has no names");
    if (rdz_pipeline_init(&v->pipe, spec->threads,
                          type == RDZ_TYPE_CHARACTER ? rdz_job_compress : rdz_job_vector,
                          (size_t)RDZ_MAX_BLOCK_SIZE, e)) {
        return 1;
    }
    v->have_pipe = 1;
    v->pipe.vtype = values_type;
    v->pipe.level = type == RDZ_TYPE_LOGICAL ? 0 : spec->level;
    if (rdz_writer_open(&v->w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION, RDZ_BLOCK_SIZE,
                        e)) {
        return 1;
    }
    if (type == RDZ_TYPE_CHARACTER) {
        rdz_emit_ctx c;
        c.v = v;
        c.spec = spec;
        if (rdz_string_encode(spec->strings, spec->policy, emit_pipeline, &c, e)) return 1;
    } else {
        do {
            size_t take = n - at < per ? n - at : per;
            int must;
            while ((s = rdz_pipeline_next(&v->pipe, &must)) != NULL && must) {
                if (consume(v, s, e)) return 1;
            }
            if (take && zb_put_bytes(&s->in, src + at * size, take * size)) {
                rdz_pipeline_unget(&v->pipe, s);
                return rdz_memory(e, "a block");
            }
            s->logical_count = take;
            rdz_pipeline_submit(&v->pipe, s, e);
            at += take;
            while ((s = rdz_pipeline_oldest(&v->pipe, 0)) != NULL) {
                if (consume(v, s, e)) return 1;
            }
            if (spec->tick && at < n) spec->tick(spec->tick_ctx);
        } while (at < n);
    }
    while ((s = rdz_pipeline_oldest(&v->pipe, 1)) != NULL) {
        if (consume(v, s, e)) return 1;
    }
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;

    objects[0].parent_id = RDZ_ROOT_PARENT_ID;
    objects[0].role = RDZ_ROLE_ROOT;
    objects[0].type_tag = type;
    objects[0].flags = factor && spec->ordered ? RDZ_OBJECT_FLAG_ORDERED : 0;
    objects[0].logical_len = n;
    objects[0].first_child = (uint32_t)factor;
    objects[0].child_count = (uint32_t)factor;
    objects[0].attribute_count = spec->names ? 1 : 0;
    objects[0].block_count = v->w.nblocks;
    if (factor) {
        first = v->w.nblocks;
        if (write_strings_plain(v, spec->levels, spec->policy, e)) return 1;
        objects[k].object_id = k;
        objects[k].role = RDZ_ROLE_LEVELS;
        objects[k].type_tag = RDZ_TYPE_CHARACTER;
        objects[k].logical_len = spec->levels->n;
        objects[k].first_block = first;
        objects[k].block_count = v->w.nblocks - first;
        k++;
    }
    if (spec->names) {
        rdz_str_source one;
        one.n = 1;
        one.ctx = NULL;
        one.key = one_key;
        one.value = one_value;
        first = v->w.nblocks;
        if (write_strings_plain(v, &one, RDZ_DICT_PLAIN, e)) return 1;
        objects[k].object_id = k;
        objects[k].role = RDZ_ROLE_ATTRIBUTE_NAME;
        objects[k].type_tag = RDZ_TYPE_CHARACTER;
        objects[k].logical_len = 1;
        objects[k].first_block = first;
        objects[k].block_count = v->w.nblocks - first;
        first = v->w.nblocks;
        if (write_strings_plain(v, spec->names, spec->policy, e)) return 1;
        objects[k + 1].object_id = k + 1;
        objects[k + 1].role = RDZ_ROLE_ATTRIBUTE_VALUE;
        objects[k + 1].type_tag = RDZ_TYPE_CHARACTER;
        objects[k + 1].logical_len = n;
        objects[k + 1].first_block = first;
        objects[k + 1].block_count = v->w.nblocks - first;
        attribute.name_object_id = k;
        attribute.value_object_id = k + 1;
        attribute.flags = RDZ_ATTRIBUTE_FLAG_NAMES;
        k += 2;
    }
    return rdz_writer_finish(&v->w, objects, k, &attribute, spec->names ? 1 : 0, NULL, 0, e);
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
