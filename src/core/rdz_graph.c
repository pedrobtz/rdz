#include <stdlib.h>
#include <string.h>

#include "rdz_content.h"
#include "rdz_graph.h"
#include "rdz_logical.h"
#include "rdz_numeric.h"

/* ---- writing ---------------------------------------------------------------------- */

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

static rdz_slot *next_slot(rdz_vec *v, rdz_error *e)
{
    /* unsupported, not a limit: automatic mode writes the object through the
       generic codec instead */
    if (v->pipe.next_submit >= RDZ_MAX_BLOCKS) {
        rdz_unsupported(e, "an object of more than a million native blocks");
        return NULL;
    }
    for (;;) {
        int must;
        rdz_slot *s = rdz_pipeline_next(&v->pipe, &must);
        if (!must) return s;
        if (consume(v, s, e)) return NULL;
    }
}

static int drain(rdz_vec *v, int wait, rdz_error *e)
{
    rdz_slot *s;
    while ((s = rdz_pipeline_oldest(&v->pipe, wait)) != NULL) {
        if (consume(v, s, e)) return 1;
    }
    return 0;
}

typedef struct {
    rdz_vec *v;
    rdz_tick_fn tick;
    void *tick_ctx;
} rdz_emit;

/* A packed string block into the pipeline, which compresses it. */
static int emit_strings(void *ctx, uint16_t encoding, uint64_t count, const uint8_t *data,
                        size_t n, rdz_error *e)
{
    rdz_emit *c = (rdz_emit *)ctx;
    rdz_slot *s = next_slot(c->v, e);
    if (!s) return 1;
    if (n && zb_put_bytes(&s->in, data, n)) {
        rdz_pipeline_unget(&c->v->pipe, s);
        return rdz_memory(e, "a block");
    }
    s->vtype = RDZ_TYPE_CHARACTER;
    s->encoding = encoding;
    s->logical_count = count;
    rdz_pipeline_submit(&c->v->pipe, s, e);
    if (drain(c->v, 0, e)) return 1;
    if (c->tick) c->tick(c->tick_ctx);
    return 0;
}

static int write_values(rdz_vec *v, const rdz_node *nodes, const rdz_node *node, rdz_tick_fn tick,
                        void *tick_ctx, rdz_error *e)
{
    uint16_t vt = node->type == RDZ_TYPE_FACTOR ? RDZ_TYPE_INTEGER : node->type;
    uint64_t levels = 0;
    size_t n = (size_t)node->length, per = rdz_vec_block_values(vt),
           size = vt == RDZ_TYPE_DOUBLE ? sizeof(double) : sizeof(int32_t), at = 0;
    const uint8_t *src = (const uint8_t *)node->values;
    if (node->type == RDZ_TYPE_FACTOR) { /* its levels, through a shared node */
        const rdz_node *l = &nodes[node->first_child];
        if (l->type == RDZ_TYPE_REFERENCE) l = &nodes[l->first_child];
        levels = l->length;
    }
    do {
        size_t take = n - at < per ? n - at : per;
        rdz_slot *s = next_slot(v, e);
        if (!s) return 1;
        /* borrowed, not copied: the caller's vector outlives the write */
        s->values = take ? src + at * size : NULL;
        s->vtype = node->type == RDZ_TYPE_FACTOR ? RDZ_TYPE_FACTOR : vt;
        s->levels = levels;
        s->logical_count = take;
        rdz_pipeline_submit(&v->pipe, s, e);
        if (v->content) rdz_content_values(v->content, src + at * size, take, size);
        at += take;
        if (drain(v, 0, e)) return 1;
        if (tick && at < n) tick(tick_ctx);
    } while (at < n);
    return 0;
}

int rdz_graph_write(rdz_vec *v, const char *path, const rdz_node *nodes, uint32_t nnodes,
                    const rdz_attribute *attributes, uint32_t nattributes, int policy,
                    int level, int threads, rdz_tick_fn tick, void *tick_ctx, rdz_error *e)
{
    zb_buf objects;
    rdz_object *o;
    uint32_t i;
    rdz_emit emit;
    int failed = 1;

    if (nnodes == 0 || nnodes > RDZ_MAX_OBJECTS) return rdz_limit(e, "object count");
    if (zb_buf_alloc(&objects, 0, 0) ||
        !zb_put_raw(&objects, (size_t)nnodes * sizeof(rdz_object))) {
        zb_buf_release(&objects);
        return rdz_memory(e, "the object directory");
    }
    o = (rdz_object *)(void *)objects.data;
    if (rdz_pipeline_init(&v->pipe, threads, rdz_job_vector, (size_t)RDZ_MAX_BLOCK_SIZE,
                          (size_t)RDZ_BLOCK_SIZE, e)) {
        goto done;
    }
    v->have_pipe = 1;
    v->pipe.level = level;
    if (rdz_writer_open(&v->w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION, RDZ_BLOCK_SIZE,
                        e)) {
        goto done;
    }
    v->w.metadata = v->metadata;
    v->w.metadata_len = v->metadata_len;
    if (v->hash_content) {
        if (!v->content) {
            /* malloc() aligns to 16 at most; the hasher needs 64 */
            v->content_mem = malloc(sizeof(rdz_content) + 63);
            if (!v->content_mem) {
                rdz_memory(e, "the content hash");
                goto done;
            }
            v->content = (rdz_content *)(void *)(((uintptr_t)v->content_mem + 63) &
                                                 ~(uintptr_t)63);
        }
        rdz_content_begin(v->content);
    }
    emit.v = v;
    emit.tick = tick;
    emit.tick_ctx = tick_ctx;
    for (i = 0; i < nnodes; i++) {
        const rdz_node *n = &nodes[i];
        memset(&o[i], 0, sizeof o[i]);
        o[i].object_id = i;
        o[i].parent_id = n->parent;
        o[i].role = n->role;
        o[i].type_tag = n->type;
        o[i].flags = n->flags;
        o[i].logical_len = n->length;
        o[i].first_child = n->first_child;
        o[i].child_count = n->child_count;
        o[i].first_attribute = n->first_attribute;
        o[i].attribute_count = n->attribute_count;
        /* blocks are written in submission order, so an object's blocks are
           numbered by the pipeline's submission count: no wait between
           objects, and every column is in flight together */
        o[i].first_block = (uint32_t)v->pipe.next_submit;
        if (v->content) rdz_content_node(v->content, n);
        switch (n->type) {
        case RDZ_TYPE_LOGICAL:
        case RDZ_TYPE_INTEGER:
        case RDZ_TYPE_DOUBLE:
        case RDZ_TYPE_FACTOR:
            if (write_values(v, nodes, n, tick, tick_ctx, e)) goto done;
            break;
        case RDZ_TYPE_CHARACTER:
            /* an attribute's name is one plain record (the validator's rule) */
            if (rdz_string_encode_hashed(n->strings,
                                         n->role == RDZ_ROLE_ATTRIBUTE_NAME ? RDZ_DICT_PLAIN
                                                                            : policy,
                                         emit_strings, &emit, v->content, e)) {
                goto done;
            }
            break;
        default: /* NULL, list, data frame: no blocks */
            break;
        }
        o[i].block_count = (uint32_t)v->pipe.next_submit - o[i].first_block;
    }
    if (drain(v, 1, e)) goto done;
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;
    if (v->content) {
        rdz_content_attributes(v->content, attributes, nattributes);
        rdz_content_end(v->content, v->w.content_hash);
        v->w.hash_scheme = RDZ_CONTENT_HASH_V1;
    }
    failed = rdz_writer_finish(&v->w, o, nnodes, attributes, nattributes, NULL, 0, e);
done:
    zb_buf_release(&objects);
    return failed;
}

/* ---- reading ---------------------------------------------------------------------- */

/* A string block into the sink; of its elements (not its dictionary
   entries), only [from, from + take). */
static int decode_strings(rdz_reader *r, const rdz_block *b, const zb_buf *rec,
                          const rdz_names_sink *sink, size_t *entries, size_t from, size_t take,
                          rdz_error *e)
{
    size_t count = (size_t)b->logical_count, bytes;
    switch (b->encoding) {
    case RDZ_ENCODING_STRING_PLAIN:
    case RDZ_ENCODING_STRING_DICT_ENTRIES:
        if (zb_size_mul(count, sizeof(rdz_str), &bytes)) return rdz_limit(e, "character values");
        zb_buf_reset(&r->records);
        if (!zb_put_raw(&r->records, bytes)) return rdz_memory(e, "character values");
        if (rdz_string_decode_records(rec->data, rec->len, count,
                                      (rdz_str *)(void *)r->records.data, e)) {
            return 1;
        }
        if (b->encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) {
            *entries += count;
            return sink->entries(sink->ctx, (const rdz_str *)(const void *)r->records.data, count,
                                 e);
        }
        /* an empty block leaves the buffer NULL, and NULL + 0 is undefined */
        return sink->plain(sink->ctx,
                           take ? (const rdz_str *)(const void *)r->records.data + from : NULL,
                           take, e);
    case RDZ_ENCODING_STRING_DICT_INDICES:
        if (zb_size_mul(count, sizeof(uint32_t), &bytes)) return rdz_limit(e, "dictionary indices");
        zb_buf_reset(&r->ids);
        if (!zb_put_raw(&r->ids, bytes)) return rdz_memory(e, "dictionary indices");
        if (rdz_string_decode_indices(rec->data, rec->len, count, *entries,
                                      (uint32_t *)(void *)r->ids.data, e)) {
            return 1;
        }
        return sink->indices(sink->ctx,
                             take ? (const uint32_t *)(const void *)r->ids.data + from : NULL,
                             take, e);
    default:
        return rdz_invalid(e, "unexpected character block encoding");
    }
}

/* A factor's level count: its levels object's length, through a reference
   (validation: a reference's target is an earlier non-reference object). */
static uint64_t factor_levels(const rdz_reader *r, const rdz_object *o)
{
    const rdz_object *levels = &r->objects[o->first_child];
    if (levels->type_tag == RDZ_TYPE_REFERENCE) levels = &r->objects[levels->first_child];
    return levels->logical_len;
}

/* A block's values into out + at; a factor's codes are checked against its
   levels here, block by block, while the block is still in cache. */
static int decode_values(const rdz_reader *r, const rdz_object *o, const rdz_block *b,
                         const zb_buf *rec, void *out, size_t at, rdz_error *e)
{
    size_t n = (size_t)b->logical_count;
    switch (o->type_tag) {
    case RDZ_TYPE_LOGICAL:
        return rdz_logical_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at, e);
    case RDZ_TYPE_INTEGER:
        return rdz_int_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at, e);
    case RDZ_TYPE_FACTOR:
        return rdz_factor_decode(rec->data, rec->len, b->encoding, n, (int32_t *)out + at,
                                 factor_levels(r, o), e);
    default:
        return rdz_dbl_decode(rec->data, rec->len, b->encoding, n, (double *)out + at, e);
    }
}

int rdz_graph_read(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks, int threads,
                   rdz_tick_fn tick, void *tick_ctx, rdz_error *e)
{
    return rdz_graph_read_some(v, r, sinks, NULL, threads, tick, tick_ctx, e);
}

int rdz_graph_read_some(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks,
                        const uint8_t *want, int threads, rdz_tick_fn tick, void *tick_ctx,
                        rdz_error *e)
{
    return rdz_graph_read_window(v, r, sinks, want, NULL, threads, tick, tick_ctx, e);
}

/* With windows: per block, whether to read it (a window's overlapping
   blocks, a windowed string object's dictionary blocks before the window's
   end, every block of an unwindowed object) and the element its values
   start at. */
typedef struct {
    uint64_t start;
    uint8_t include;
} rdz_block_plan;

static int plan_blocks(rdz_vec *v, const rdz_reader *r, const rdz_window *windows,
                       rdz_error *e)
{
    rdz_block_plan *bp;
    uint32_t k, b;
    zb_buf_reset(&v->block_plan);
    if (zb_put_zeros(&v->block_plan, (size_t)r->nblocks * sizeof(rdz_block_plan))) {
        return rdz_memory(e, "the block plan");
    }
    bp = (rdz_block_plan *)(void *)v->block_plan.data;
    for (k = 0; k < r->nobjects; k++) {
        const rdz_object *o = &r->objects[k];
        uint64_t pos = 0;
        for (b = o->first_block; b < o->first_block + o->block_count; b++) {
            const rdz_block *blk = &r->blocks[b];
            bp[b].start = pos;
            if (!windows || !windows[k].on) {
                bp[b].include = 1;
            } else if (blk->encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) {
                bp[b].include = pos < windows[k].hi; /* indices refer back to them */
                continue;                            /* entries are not elements */
            } else {
                bp[b].include = pos < windows[k].hi && pos + blk->logical_count > windows[k].lo;
            }
            pos += blk->logical_count;
        }
    }
    return 0;
}

int rdz_graph_read_window(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks,
                          const uint8_t *want, const rdz_window *windows, int threads,
                          rdz_tick_fn tick, void *tick_ctx, rdz_error *e)
{
    uint32_t next = 0, object = 0, owner = 0;
    size_t filled = 0, entries = 0;
    void *dest = NULL;
    const rdz_names_sink *sink = NULL;
    const rdz_block_plan *bp = NULL;
    if (windows) {
        uint32_t i;
        for (i = 0; i < r->nobjects; i++) { /* callers check too; the core does not rely on it */
            if (windows[i].on &&
                !(windows[i].lo <= windows[i].hi && windows[i].hi <= r->objects[i].logical_len)) {
                return rdz_limit(e, "row range");
            }
        }
        if (plan_blocks(v, r, windows, e)) return 1;
        bp = (const rdz_block_plan *)(const void *)v->block_plan.data;
    }
    if (v->have_pipe) {
        rdz_pipeline_free(&v->pipe);
        v->have_pipe = 0;
    }
    if (rdz_pipeline_init(&v->pipe, r->nblocks > 1 ? threads : 1, rdz_job_decode,
                          (size_t)RDZ_MAX_BLOCK_SIZE, (size_t)r->block_size, e)) {
        return 1;
    }
    v->have_pipe = 1;
    for (;;) {
        rdz_slot *s;
        const rdz_block *b;
        uint32_t index;
        const rdz_object *o;
        while (next < r->nblocks && v->pipe.next_submit - v->pipe.next_consume < v->pipe.nslots) {
            int must;
            if (want) {
                /* the object owning block `next` (blocks run in object
                   order); an unwanted one's blocks are skipped whole */
                while (owner < r->nobjects &&
                       next >= r->objects[owner].first_block + r->objects[owner].block_count) {
                    owner++;
                }
                if (owner < r->nobjects && !want[owner]) {
                    next = r->objects[owner].first_block + r->objects[owner].block_count;
                    continue;
                }
            }
            if (bp && !bp[next].include) { /* outside its object's window */
                next++;
                continue;
            }
            s = rdz_pipeline_next(&v->pipe, &must);
            if (rdz_reader_read_stored(r, next, &s->in, e)) {
                rdz_pipeline_unget(&v->pipe, s);
                return 1;
            }
            s->block = &r->blocks[next];
            s->index = next++;
            rdz_pipeline_submit(&v->pipe, s, e);
        }
        s = rdz_pipeline_oldest(&v->pipe, 1);
        if (!s) break;
        if (s->failed) {
            *e = s->e;
            return 1;
        }
        b = (const rdz_block *)s->block;
        index = s->index;
        /* the object owning this block: validation made blocks contiguous in
           object order, so it is this object or a later one */
        while (object < r->nobjects &&
               index >= r->objects[object].first_block + r->objects[object].block_count) {
            object++;
            filled = 0;
            entries = 0;
            dest = NULL;
            sink = NULL;
        }
        if (object == r->nobjects) return rdz_invalid(e, "a block belongs to no object");
        o = &r->objects[object];
        if (windows && windows[object].on) {
            /* the overlap of the block's elements with the window */
            uint64_t start = bp[index].start, n = b->logical_count, lo = windows[object].lo,
                     hi = windows[object].hi, a = start > lo ? start : lo,
                     z = start + n < hi ? start + n : hi;
            if (o->type_tag == RDZ_TYPE_CHARACTER) {
                if (!sink) sink = sinks->strings(sinks->ctx, object);
                if (b->encoding == RDZ_ENCODING_STRING_DICT_ENTRIES) {
                    a = start;
                    z = start; /* none of its elements: its entries go to the sink whole */
                }
                if (decode_strings(r, b, s->result, sink, &entries, (size_t)(a - start),
                                   (size_t)(z - a), e)) {
                    return 1;
                }
            } else {
                size_t width = o->type_tag == RDZ_TYPE_DOUBLE ? 8 : 4;
                if (!dest) dest = sinks->values(sinks->ctx, object);
                if (a == start && z == start + n) {
                    if (decode_values(r, o, b, s->result, dest, (size_t)(start - lo), e)) {
                        return 1;
                    }
                } else {
                    zb_buf_reset(&v->scratch);
                    if (!zb_put_raw(&v->scratch, (size_t)n * width + 8)) {
                        return rdz_memory(e, "a block");
                    }
                    if (decode_values(r, o, b, s->result, v->scratch.data, 0, e)) return 1;
                    memcpy((uint8_t *)dest + (size_t)(a - lo) * width,
                           v->scratch.data + (size_t)(a - start) * width, (size_t)(z - a) * width);
                }
            }
        } else if (o->type_tag == RDZ_TYPE_CHARACTER) {
            if (!sink) sink = sinks->strings(sinks->ctx, object);
            if (decode_strings(r, b, s->result, sink, &entries, 0, (size_t)b->logical_count, e)) {
                return 1;
            }
        } else {
            if (!dest) dest = sinks->values(sinks->ctx, object);
            if (b->logical_count > o->logical_len - filled) {
                return rdz_invalid(e, "native payload length mismatch");
            }
            if (decode_values(r, o, b, s->result, dest, filled, e)) return 1;
            filled += (size_t)b->logical_count;
        }
        rdz_pipeline_release(&v->pipe, s);
        if (tick && (next < r->nblocks || v->pipe.next_consume != v->pipe.next_submit)) {
            tick(tick_ctx);
        }
    }
    rdz_pipeline_free(&v->pipe);
    v->have_pipe = 0;
    return 0;
}
