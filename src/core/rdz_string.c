#include <string.h>

#include <zubin/rw.h>

#include "rdz_content.h"
#include "rdz_string.h"

#define AUTO_SAMPLE ((size_t)16 * 1024)
#define AUTO_MAX_DISTINCT_SHARE 0.75
#define MAX_DICTIONARY_ENTRIES ((size_t)1 << 22)
#define DICT_INDEX_HEADER RDZ_DICT_INDEX_HEADER_LEN

/* ---- an identity map: key -> value, open addressing, keys never 0 ---------------- */

typedef struct {
    uintptr_t key;
    uint32_t value;
    uint32_t used;
} rdz_slot_kv;

typedef struct {
    zb_buf mem; /* the slots */
    size_t cap; /* a power of two */
    size_t len;
} rdz_map;

/* fmix64 from MurmurHash3: addresses are aligned, so the low bits alone
   would collide. */
static size_t rdz_mix(uintptr_t key)
{
    uint64_t h = (uint64_t)key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return (size_t)h;
}

static int map_init(rdz_map *m, size_t cap, rdz_error *e)
{
    size_t bytes;
    m->cap = 16;
    while (m->cap < cap) m->cap <<= 1;
    m->len = 0;
    if (zb_size_mul(m->cap, sizeof(rdz_slot_kv), &bytes) || zb_buf_alloc(&m->mem, 0, 0) ||
        zb_put_zeros(&m->mem, bytes)) {
        zb_buf_release(&m->mem);
        return rdz_memory(e, "a string dictionary");
    }
    return 0;
}

static void map_clear(rdz_map *m)
{
    memset(m->mem.data, 0, m->mem.len);
    m->len = 0;
}

static rdz_slot_kv *map_slots(rdz_map *m) { return (rdz_slot_kv *)(void *)m->mem.data; }

/* The slot holding key, or the empty slot where it would go. */
static rdz_slot_kv *map_find(rdz_map *m, uintptr_t key)
{
    size_t i = rdz_mix(key) & (m->cap - 1);
    rdz_slot_kv *s = map_slots(m);
    while (s[i].used && s[i].key != key) i = (i + 1) & (m->cap - 1);
    return &s[i];
}

static int map_grow(rdz_map *m, rdz_error *e)
{
    rdz_map bigger;
    rdz_slot_kv *old = map_slots(m);
    size_t i;
    if (map_init(&bigger, m->cap * 2, e)) return 1;
    for (i = 0; i < m->cap; i++) {
        if (old[i].used) {
            *map_find(&bigger, old[i].key) = old[i];
            bigger.len++;
        }
    }
    zb_buf_release(&m->mem);
    *m = bigger;
    return 0;
}

/* The slot for key, inserting it with value `fresh` when absent and
   setting *inserted. */
static rdz_slot_kv *map_get_or_put(rdz_map *m, uintptr_t key, uint32_t fresh, int *inserted,
                                   rdz_error *e)
{
    rdz_slot_kv *s;
    if (2 * (m->len + 1) > m->cap && map_grow(m, e)) return NULL;
    s = map_find(m, key);
    *inserted = !s->used;
    if (!s->used) {
        s->used = 1;
        s->key = key;
        s->value = fresh;
        m->len++;
    }
    return s;
}

/* ---- records ---------------------------------------------------------------------- */

static int push_record(zb_buf *out, const rdz_str *v, rdz_error *e)
{
    uint8_t *p = zb_put_raw(out, RDZ_STRING_RECORD_HEADER + v->len);
    if (!p) return rdz_memory(e, "a character block");
    p[0] = v->tag;
    zb_wr_u32le(p + 1, (uint32_t)v->len);
    if (v->len) memcpy(p + RDZ_STRING_RECORD_HEADER, v->bytes, v->len);
    return 0;
}

static int record_len(const rdz_str *v, size_t *len, rdz_error *e)
{
    *len = RDZ_STRING_RECORD_HEADER + v->len;
    if (*len > RDZ_BLOCK_SIZE) return rdz_limit(e, "character value block");
    return 0;
}

static int encode_plain(const rdz_str_source *src, size_t from, rdz_emit_fn emit, void *ectx,
                        zb_buf *buf, rdz_content *content, rdz_error *e)
{
    size_t i, count = 0;
    int emitted = 0;
    zb_buf_reset(buf);
    for (i = from; i < src->n; i++) {
        rdz_str v;
        size_t rec;
        if (src->value(src->ctx, i, &v, e) || record_len(&v, &rec, e)) return 1;
        if (content) rdz_content_string(content, &v);
        if (count != 0 && buf->len + rec > RDZ_BLOCK_SIZE) {
            if (emit(ectx, RDZ_ENCODING_STRING_PLAIN, count, buf->data, buf->len, e)) return 1;
            zb_buf_reset(buf);
            count = 0;
            emitted = 1;
        }
        if (push_record(buf, &v, e)) return 1;
        count++;
    }
    if (count != 0 || !emitted) {
        return emit(ectx, RDZ_ENCODING_STRING_PLAIN, count, buf->data, buf->len, e);
    }
    return 0;
}

/* Exact for a vector no larger than the sample; otherwise the bias-corrected
   Chao1 estimate from a fixed-seed sample of positions (string.rs). */
static int distinct_share(const rdz_str_source *src, double *share, rdz_error *e)
{
    rdz_map counts;
    size_t n = src->n, i, f1 = 0, f2 = 0;
    int inserted;
    if (n == 0) {
        *share = 0.0;
        return 0;
    }
    if (map_init(&counts, 2 * (n <= AUTO_SAMPLE ? n : AUTO_SAMPLE), e)) return 1;
    if (n <= AUTO_SAMPLE) {
        for (i = 0; i < n; i++) {
            if (!map_get_or_put(&counts, src->key(src->ctx, i), 0, &inserted, e)) goto fail;
        }
        *share = (double)counts.len / (double)n;
    } else {
        uint64_t state = 0x9e3779b97f4a7c15ull;
        rdz_slot_kv *s;
        double estimate, ones, twos;
        for (i = 0; i < AUTO_SAMPLE; i++) {
            size_t pos;
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            pos = (size_t)((state * 0x2545f4914f6cdd1dull) % (uint64_t)n);
            s = map_get_or_put(&counts, src->key(src->ctx, pos), 0, &inserted, e);
            if (!s) goto fail;
            s->value++;
        }
        s = map_slots(&counts);
        for (i = 0; i < counts.cap; i++) {
            if (s[i].used && s[i].value == 1) f1++;
            if (s[i].used && s[i].value == 2) f2++;
        }
        ones = (double)f1;
        twos = (double)f2;
        estimate = (double)counts.len + ones * (ones - 1.0) / (2.0 * (twos + 1.0));
        *share = estimate / (double)n;
        if (*share > 1.0) *share = 1.0;
    }
    zb_buf_release(&counts.mem);
    return 0;
fail:
    zb_buf_release(&counts.mem);
    return 1;
}

static int encode_indices(const uint32_t *ids, size_t n, zb_buf *out, rdz_error *e)
{
    uint32_t low = 0xffffffffu, high = 0, base, range;
    size_t width, i;
    uint8_t *p;
    for (i = 0; i < n; i++) {
        if (ids[i] < low) low = ids[i];
        if (ids[i] > high) high = ids[i];
    }
    base = n ? low : 0;
    range = high >= base ? high - base : 0;
    width = range <= 0xffu ? 1 : range <= 0xffffu ? 2 : 4;
    zb_buf_reset(out);
    p = zb_put_raw(out, DICT_INDEX_HEADER + n * width);
    if (!p) return rdz_memory(e, "a dictionary index block");
    memset(p, 0, DICT_INDEX_HEADER);
    p[0] = (uint8_t)width;
    zb_wr_u32le(p + 4, base);
    p += DICT_INDEX_HEADER;
    for (i = 0; i < n; i++) {
        uint32_t d = ids[i] - base;
        if (width == 1) p[i] = (uint8_t)d;
        else if (width == 2) zb_wr_u16le(p + 2 * i, (uint16_t)d);
        else zb_wr_u32le(p + 4 * i, d);
    }
    return 0;
}

int rdz_string_encode(const rdz_str_source *src, int policy, rdz_emit_fn emit, void *ectx,
                      rdz_error *e)
{
    return rdz_string_encode_hashed(src, policy, emit, ectx, NULL, e);
}

int rdz_string_encode_hashed(const rdz_str_source *src, int policy, rdz_emit_fn emit, void *ectx,
                             rdz_content *content, rdz_error *e)
{
    rdz_map ids;
    zb_buf buf, chunk_ids, first_seen, digests; /* digests: per dictionary id */
    size_t start = 0, n = src->n;
    uint32_t next_id = 0;
    int failed = 1;

    zb_buf_alloc(&buf, 0, 0);
    if (policy == RDZ_DICT_AUTO) {
        double share;
        if (distinct_share(src, &share, e)) goto done_buf;
        policy = share < AUTO_MAX_DISTINCT_SHARE ? RDZ_DICT_GLOBAL : RDZ_DICT_PLAIN;
    }
    if (policy == RDZ_DICT_PLAIN || n == 0) {
        failed = encode_plain(src, 0, emit, ectx, &buf, content, e);
        goto done_buf;
    }
    if (map_init(&ids, 1024, e)) goto done_buf;
    zb_buf_alloc(&chunk_ids, 0, 0);
    zb_buf_alloc(&first_seen, 0, 0);
    zb_buf_alloc(&digests, 0, 0);
    while (start < n) {
        size_t end = n - start < RDZ_DICT_CHUNK_VALUES ? n : start + RDZ_DICT_CHUNK_VALUES;
        size_t i, nfirst, entry_count = 0;
        uint32_t *cid;
        size_t *fs;
        if (policy == RDZ_DICT_BLOCK) map_clear(&ids);
        zb_buf_reset(&chunk_ids);
        zb_buf_reset(&first_seen);
        if (!zb_put_raw(&chunk_ids, (end - start) * sizeof(uint32_t))) {
            rdz_memory(e, "dictionary ids");
            goto done;
        }
        cid = (uint32_t *)(void *)chunk_ids.data;
        for (i = start; i < end; i++) {
            uint32_t id;
            int inserted;
            rdz_slot_kv *slot_kv = map_get_or_put(&ids, src->key(src->ctx, i), next_id, &inserted, e);
            if (!slot_kv) goto done;
            id = slot_kv->value;
            if (inserted) {
                size_t *slot = (size_t *)(void *)zb_put_raw(&first_seen, sizeof(size_t));
                if (!slot) {
                    rdz_memory(e, "dictionary entries");
                    goto done;
                }
                *slot = i;
                if (next_id == 0xffffffffu) {
                    rdz_limit(e, "string dictionary size");
                    goto done;
                }
                next_id++;
            }
            cid[i - start] = id;
        }
        if (ids.len > MAX_DICTIONARY_ENTRIES) {
            /* nothing from this chunk has been emitted yet */
            failed = encode_plain(src, start, emit, ectx, &buf, content, e);
            goto done;
        }
        fs = (size_t *)(void *)first_seen.data;
        nfirst = first_seen.len / sizeof(size_t);
        zb_buf_reset(&buf);
        for (i = 0; i < nfirst; i++) {
            rdz_str v;
            size_t rec;
            if (src->value(src->ctx, fs[i], &v, e) || record_len(&v, &rec, e)) goto done;
            if (content) { /* entries come in id order */
                uint64_t d = rdz_string_digest(&v);
                if (zb_put_bytes(&digests, &d, sizeof d)) {
                    rdz_memory(e, "dictionary digests");
                    goto done;
                }
            }
            if (entry_count != 0 && buf.len + rec > RDZ_BLOCK_SIZE) {
                if (emit(ectx, RDZ_ENCODING_STRING_DICT_ENTRIES, entry_count, buf.data, buf.len, e)) {
                    goto done;
                }
                zb_buf_reset(&buf);
                entry_count = 0;
            }
            if (push_record(&buf, &v, e)) goto done;
            entry_count++;
        }
        if (entry_count != 0 &&
            emit(ectx, RDZ_ENCODING_STRING_DICT_ENTRIES, entry_count, buf.data, buf.len, e)) {
            goto done;
        }
        if (content) {
            rdz_content_digests(content, (const uint64_t *)(const void *)digests.data, cid,
                                end - start);
        }
        if (encode_indices(cid, end - start, &buf, e) ||
            emit(ectx, RDZ_ENCODING_STRING_DICT_INDICES, end - start, buf.data, buf.len, e)) {
            goto done;
        }
        start = end;
    }
    failed = 0;
done:
    zb_buf_release(&chunk_ids);
    zb_buf_release(&first_seen);
    zb_buf_release(&digests);
    zb_buf_release(&ids.mem);
done_buf:
    zb_buf_release(&buf);
    return failed;
}

int rdz_string_decode_records(const uint8_t *block, size_t len, size_t count, rdz_str *out,
                              rdz_error *e)
{
    size_t at = 0, i;
    for (i = 0; i < count; i++) {
        uint8_t tag;
        size_t n;
        if (len - at < RDZ_STRING_RECORD_HEADER) {
            return rdz_invalid(e, at < len ? "truncated character bytes" : "truncated character record");
        }
        tag = block[at];
        n = zb_rd_u32le(block + at + 1);
        at += RDZ_STRING_RECORD_HEADER;
        if (n > len - at) return rdz_invalid(e, "truncated character bytes"); /* GUARD: string-record-length */
        if (tag == RDZ_STR_NA) {
            if (n != 0) return rdz_invalid(e, "missing character record has nonzero length");
        } else if (tag > RDZ_STR_BYTES) {
            return rdz_invalid(e, "invalid character encoding tag");
        }
        out[i].tag = tag;
        out[i].bytes = block + at;
        out[i].len = n;
        at += n;
    }
    if (at != len) return rdz_invalid(e, "character block has trailing record bytes");
    return 0;
}

int rdz_string_decode_indices(const uint8_t *block, size_t len, size_t count,
                              size_t dictionary_len, uint32_t *out, rdz_error *e)
{
    size_t width, i;
    uint64_t base, high = 0;
    if (len < DICT_INDEX_HEADER) return rdz_invalid(e, "truncated dictionary index header");
    width = block[0];
    if (width != 1 && width != 2 && width != 4) return rdz_invalid(e, "invalid dictionary index width");
    if (block[1] || block[2] || block[3]) {
        return rdz_invalid(e, "nonzero dictionary index reserved bytes");
    }
    base = zb_rd_u32le(block + 4);
    if (count > (len - DICT_INDEX_HEADER) / width || DICT_INDEX_HEADER + count * width != len) {
        return rdz_invalid(e, "dictionary index block length mismatch");
    }
    for (i = 0; i < count; i++) {
        const uint8_t *p = block + DICT_INDEX_HEADER + i * width;
        uint32_t v = width == 1 ? p[0] : width == 2 ? zb_rd_u16le(p) : zb_rd_u32le(p);
        if (v > high) high = v;
        out[i] = v;
    }
    if (count != 0 && (base + high >= (uint64_t)dictionary_len || base + high > 0xffffffffull)) { /* GUARD: dict-index-range */
        return rdz_invalid(e, "dictionary index refers to an undefined entry");
    }
    for (i = 0; i < count; i++) out[i] += (uint32_t)base;
    return 0;
}
