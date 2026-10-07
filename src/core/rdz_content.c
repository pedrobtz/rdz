#include <string.h>

#include <zubin/rw.h>

#include "rdz_content.h"

static void flush(rdz_content *c)
{
    if (c->at) zuf_hasher_update(&c->h, c->buf, c->at);
    c->at = 0;
}

static void put(rdz_content *c, const void *data, size_t n)
{
    if (c->at + n > sizeof c->buf) flush(c);
    if (n > sizeof c->buf) {
        zuf_hasher_update(&c->h, data, n);
        return;
    }
    memcpy(c->buf + c->at, data, n);
    c->at += n;
}

static int little_endian(void)
{
    const uint16_t one = 1;
    uint8_t b;
    memcpy(&b, &one, 1);
    return b == 1;
}

void rdz_content_begin(rdz_content *c)
{
    static const uint8_t magic[8] = {'R', 'D', 'Z', 'H', 1, 'N', 0, 0};
    zuf_hasher_init(&c->h, 0);
    c->at = 0;
    put(c, magic, sizeof magic);
}

void rdz_content_node(rdz_content *c, const rdz_node *n)
{
    uint8_t rec[36];
    zb_wr_u16le(rec, n->type);
    zb_wr_u16le(rec + 2, n->role);
    zb_wr_u32le(rec + 4, n->flags);
    zb_wr_u32le(rec + 8, n->parent);
    zb_wr_u64le(rec + 12, n->length);
    zb_wr_u32le(rec + 20, n->first_child);
    zb_wr_u32le(rec + 24, n->child_count);
    zb_wr_u32le(rec + 28, n->first_attribute);
    zb_wr_u32le(rec + 32, n->attribute_count);
    put(c, rec, sizeof rec);
}

void rdz_content_values(rdz_content *c, const void *values, size_t n, size_t width)
{
    const uint8_t *src = (const uint8_t *)values;
    size_t i, k;
    if (little_endian()) {
        flush(c);
        if (n) zuf_hasher_update(&c->h, values, n * width);
        return;
    }
    for (i = 0; i < n; i++) {
        uint8_t le[8];
        for (k = 0; k < width; k++) le[k] = src[i * width + width - 1 - k];
        put(c, le, width);
    }
}

uint64_t rdz_string_digest(const rdz_str *s)
{
    uint8_t rec[256];
    rec[0] = s->tag;
    zb_wr_u32le(rec + 1, (uint32_t)s->len);
    if (s->len <= sizeof rec - 5) { /* one shot: a streaming state costs more than a short string */
        if (s->len) memcpy(rec + 5, s->bytes, s->len);
        return zuf_hash64(rec, 5 + s->len);
    } else {
        zuf_hasher h;
        zuf_hasher_init(&h, 0);
        zuf_hasher_update(&h, rec, 5);
        zuf_hasher_update(&h, s->bytes, s->len);
        return zuf_hasher_digest64(&h);
    }
}

void rdz_content_digest(rdz_content *c, uint64_t digest)
{
    uint8_t le[8];
    zb_wr_u64le(le, digest);
    put(c, le, 8);
}

void rdz_content_digests(rdz_content *c, const uint64_t *digests, const uint32_t *ids, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t room = (sizeof c->buf - c->at) / 8, k, take = n - i < room ? n - i : room;
        uint8_t *p = c->buf + c->at;
        if (!take) {
            flush(c);
            continue;
        }
        for (k = 0; k < take; k++) zb_wr_u64le(p + 8 * k, digests[ids[i + k]]);
        c->at += 8 * take;
        i += take;
    }
}

void rdz_content_string(rdz_content *c, const rdz_str *s)
{
    rdz_content_digest(c, rdz_string_digest(s));
}

/* A record is exactly the bytes rdz_string_digest() hashes (tag, u32le
   length, the string), and XXH3-64 in one shot over them equals its
   two-part digest: hashed where the encoder wrote them, long after, they
   cost no copy and no load waiting on its own store. */
static size_t record_digest(const uint8_t *rec, uint64_t *d)
{
    size_t len = RDZ_STRING_RECORD_HEADER + zb_rd_u32le(rec + 1);
    *d = zuf_hash64(rec, len);
    return len;
}

size_t rdz_record_digests(const uint8_t *records, size_t count, uint64_t *out)
{
    size_t at = 0, k;
    for (k = 0; k < count; k++) at += record_digest(records + at, &out[k]);
    return at;
}

void rdz_content_records(rdz_content *c, const uint8_t *records, size_t count)
{
    size_t at = 0;
    while (count) {
        size_t room = (sizeof c->buf - c->at) / 8, k, take = count < room ? count : room;
        uint8_t *p = c->buf + c->at;
        if (!take) {
            flush(c);
            continue;
        }
        for (k = 0; k < take; k++) {
            uint64_t d;
            at += record_digest(records + at, &d);
            zb_wr_u64le(p + 8 * k, d);
        }
        c->at += 8 * take;
        count -= take;
    }
}

void rdz_content_attributes(rdz_content *c, const rdz_attribute *a, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t rec[20];
        zb_wr_u32le(rec, a[i].owner_id);
        zb_wr_u32le(rec + 4, a[i].name_object_id);
        zb_wr_u32le(rec + 8, a[i].value_object_id);
        zb_wr_u32le(rec + 12, a[i].ordinal);
        zb_wr_u32le(rec + 16, a[i].flags);
        put(c, rec, sizeof rec);
    }
}

void rdz_content_end(rdz_content *c, uint8_t out[16])
{
    zuf_digest128 d;
    flush(c);
    d = zuf_hasher_digest128(&c->h);
    zb_wr_u64le(out, d.low);
    zb_wr_u64le(out + 8, d.high);
}
