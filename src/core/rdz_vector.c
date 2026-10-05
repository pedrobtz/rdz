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

int rdz_vec_write(rdz_vec *v, const char *path, uint16_t type, const void *values, size_t n,
                  const rdz_str_source *names, int policy, int level, int threads,
                  rdz_tick_fn tick, void *tick_ctx, rdz_error *e)
{
    size_t per = rdz_vec_block_values(type), size = elem_size(type), at = 0;
    const uint8_t *src = (const uint8_t *)values;
    rdz_object objects[3];
    rdz_attribute attribute;
    rdz_slot *s;
    uint32_t vblocks, name_first, value_first;

    memset(objects, 0, sizeof objects);
    memset(&attribute, 0, sizeof attribute);
    if (names && names->n != n) return rdz_invalid(e, "names length does not match the vector");
    if (rdz_pipeline_init(&v->pipe, threads, rdz_job_vector, (size_t)RDZ_MAX_BLOCK_SIZE, e)) {
        return 1;
    }
    v->have_pipe = 1;
    v->pipe.vtype = type;
    v->pipe.level = type == RDZ_TYPE_LOGICAL ? 0 : level;
    if (rdz_writer_open(&v->w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION, RDZ_BLOCK_SIZE,
                        e)) {
        return 1;
    }
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
        if (tick && at < n) tick(tick_ctx);
    } while (at < n);
    while ((s = rdz_pipeline_oldest(&v->pipe, 1)) != NULL) {
        if (consume(v, s, e)) return 1;
    }
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;
    vblocks = v->w.nblocks;

    objects[0].parent_id = RDZ_ROOT_PARENT_ID;
    objects[0].role = RDZ_ROLE_ROOT;
    objects[0].type_tag = type;
    objects[0].logical_len = n;
    objects[0].attribute_count = names ? 1 : 0;
    objects[0].block_count = vblocks;
    if (names) {
        rdz_str_source one;
        one.n = 1;
        one.ctx = NULL;
        one.key = one_key;
        one.value = one_value;
        name_first = v->w.nblocks;
        if (rdz_string_encode(&one, RDZ_DICT_PLAIN, emit_block, &v->w, e)) return 1;
        value_first = v->w.nblocks;
        if (rdz_string_encode(names, policy, emit_block, &v->w, e)) return 1;
        objects[1].object_id = 1;
        objects[1].role = RDZ_ROLE_ATTRIBUTE_NAME;
        objects[1].type_tag = RDZ_TYPE_CHARACTER;
        objects[1].logical_len = 1;
        objects[1].first_block = name_first;
        objects[1].block_count = value_first - name_first;
        objects[2].object_id = 2;
        objects[2].role = RDZ_ROLE_ATTRIBUTE_VALUE;
        objects[2].type_tag = RDZ_TYPE_CHARACTER;
        objects[2].logical_len = n;
        objects[2].first_block = value_first;
        objects[2].block_count = v->w.nblocks - value_first;
        attribute.name_object_id = 1;
        attribute.value_object_id = 2;
        attribute.flags = RDZ_ATTRIBUTE_FLAG_NAMES;
    }
    return rdz_writer_finish(&v->w, objects, names ? 3 : 1, &attribute, names ? 1 : 0, NULL, 0,
                             e);
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
        return rdz_int_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at, e);
    default:
        return rdz_dbl_decode(rec->data, rec->len, b->encoding, n, (double *)out + at, e);
    }
}

int rdz_vec_read(rdz_vec *v, rdz_reader *r, void *out, int threads, rdz_tick_fn tick,
                 void *tick_ctx, rdz_error *e)
{
    const rdz_object *root;
    uint16_t type;
    size_t n, at = 0;
    uint32_t next, end;
    if (rdz_vec_shape(r, &type, &n, e)) return 1;
    root = &r->objects[0];
    next = root->first_block;
    end = root->first_block + root->block_count;
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
