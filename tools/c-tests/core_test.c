/*
 * The R-free test harness for src/core (plan-c.md section 4): compiled
 * without R, with -DRDZ_STANDALONE, by tools/run-c-tests, which runs it from
 * the package root under ASan and UBSan where the compiler has them.
 *
 * Against the Rust reference corpus (tests/testthat/fixtures/rust/):
 *   - every fixture opens, reports manifest.tsv's codec, block count, size
 *     and block encodings, and every block's checksum verifies;
 *   - every generic fixture, rewritten by the C writer from its own payload
 *     and synopsis, is byte for byte the file the Rust writer wrote;
 *   - every single-byte change and every truncation of a small fixture is
 *     an error, from opening or from reading a block, and never a crash.
 * And the record layouts match their zubin specifications.
 */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <zubin/rw.h>

#include "rdz_alp.h"
#include "rdz_container.h"
#include "rdz_graph.h"
#include "rdz_logical.h"
#include "rdz_native.h"
#include "rdz_numeric.h"
#include "rdz_pipeline.h"
#include "rdz_vector.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

#define FIXTURES "tests/testthat/fixtures/rust/"

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)size;
    return buf;
}

/* Opens, reads every block; 0 when all is valid. */
static int read_all(const uint8_t *data, size_t n, rdz_error *e)
{
    rdz_reader r;
    zb_buf block;
    uint32_t i;
    int failed = 0;
    if (rdz_reader_open_memory(&r, data, n, e)) return 1;
    zb_buf_alloc(&block, 0, (size_t)RDZ_MAX_BLOCK_SIZE);
    for (i = 0; i < r.nblocks && !failed; i++) failed = rdz_reader_read_block(&r, i, &block, e);
    zb_buf_release(&block);
    rdz_reader_close(&r);
    return failed;
}

/* The same bytes but the header's writer field and so its checksum
   (bytes 20 to 31): the Rust reference recorded no writer. */
static int same_but_writer(const uint8_t *a, size_t n, const uint8_t *b, size_t m)
{
    return a && b && n == m && n >= RDZ_HEADER_LEN && memcmp(a, b, 20) == 0 &&
           memcmp(a + RDZ_HEADER_LEN, b + RDZ_HEADER_LEN, n - RDZ_HEADER_LEN) == 0;
}

static void test_records(void)
{
    const char *record = NULL;
    int bad = rdz_records_check(&record);
    CHECK(bad == 0, "the %s layout disagrees with its offsets", record ? record : "?");
}

/* The sorted distinct encodings of r, as "0 2 4". */
static void encodings_of(const rdz_reader *r, char *out, size_t cap)
{
    int seen[16] = {0};
    uint32_t i;
    size_t at = 0;
    out[0] = '\0';
    for (i = 0; i < r->nblocks; i++) {
        if (r->blocks[i].encoding < 16) seen[r->blocks[i].encoding] = 1;
    }
    for (i = 0; i < 16; i++) {
        if (seen[i] && at + 4 < cap) at += (size_t)snprintf(out + at, cap - at, at ? " %u" : "%u", i);
    }
}

static void test_fixture(const char *name, const char *codec, unsigned long blocks,
                         const char *encodings, unsigned long long bytes, const char *tmpdir)
{
    char path[512], copy[600], have[64];
    rdz_reader r;
    rdz_error e;
    zb_buf block, payload;
    uint32_t i;
    size_t n = 0, m = 0;
    uint8_t *original, *rewritten;
    unsigned want_codec = strcmp(codec, "native_v1") == 0 ? RDZ_CODEC_NATIVE_V1 : RDZ_CODEC_R_SERIAL_V3;

    snprintf(path, sizeof path, FIXTURES "%s.rdz", name);
    if (rdz_reader_open(&r, path, &e)) {
        CHECK(0, "%s: %s", name, e.message);
        return;
    }
    CHECK(r.codec_id == want_codec, "%s: codec %u", name, r.codec_id);
    CHECK(r.nblocks == blocks, "%s: %u blocks, manifest %lu", name, r.nblocks, blocks);
    CHECK(r.file_bytes == bytes, "%s: %llu bytes", name, (unsigned long long)r.file_bytes);
    encodings_of(&r, have, sizeof have);
    CHECK(strcmp(have, encodings) == 0, "%s: encodings %s, manifest %s", name, have, encodings);

    zb_buf_alloc(&block, 0, (size_t)RDZ_MAX_BLOCK_SIZE);
    zb_buf_alloc(&payload, 0, 0);
    for (i = 0; i < r.nblocks; i++) {
        if (rdz_reader_read_block(&r, i, &block, &e)) {
            CHECK(0, "%s block %u: %s", name, i, e.message);
            break;
        }
        zb_put_bytes(&payload, block.data, block.len);
    }
    CHECK(payload.len == r.payload_bytes, "%s: payload %zu bytes", name, payload.len);

    if (r.codec_id == RDZ_CODEC_R_SERIAL_V3) {
        snprintf(copy, sizeof copy, "%s/rewrite-%s.rdz", tmpdir, name);
        if (rdz_write_generic(copy, payload.data, payload.len, r.synopsis, r.synopsis_len, &e)) {
            CHECK(0, "%s: rewrite: %s", name, e.message);
        } else {
            original = slurp(path, &n);
            rewritten = slurp(copy, &m);
            CHECK(same_but_writer(original, n, rewritten, m),
                  "%s: the C writer's bytes differ from the Rust writer's", name);
            free(original);
            free(rewritten);
            remove(copy);
        }
    }
    zb_buf_release(&block);
    zb_buf_release(&payload);
    rdz_reader_close(&r);
}

static void test_manifest(const char *tmpdir)
{
    FILE *f = fopen(FIXTURES "manifest.tsv", "r");
    char line[4096];
    int rows = 0;
    CHECK(f != NULL, "cannot open the manifest; run from the package root");
    if (!f) return;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        /* name mode policy codec blocks encodings bytes sha256 commit spec */
        char *field[10];
        int k = 0;
        char *p = line;
        while (k < 10 && p) {
            field[k++] = p;
            p = strchr(p, '\t');
            if (p) *p++ = '\0';
        }
        if (k < 7) continue;
        test_fixture(field[0], field[3], strtoul(field[4], NULL, 10), field[5],
                     strtoull(field[6], NULL, 10), tmpdir);
        rows++;
    }
    fclose(f);
    CHECK(rows >= 33, "only %d fixtures in the manifest", rows);
}

static void test_mutations(const char *dir, const char *name)
{
    char path[512];
    size_t n = 0, i;
    uint8_t *data;
    rdz_error e;
    int undetected = 0;
    snprintf(path, sizeof path, "%s%s.rdz", dir, name);
    data = slurp(path, &n);
    CHECK(data != NULL, "cannot read %s", path);
    if (!data) return;
    CHECK(read_all(data, n, &e) == 0, "%s does not read: %s", name, e.message);
    for (i = 0; i < n; i++) {
        uint8_t saved = data[i];
        data[i] ^= 0x5a;
        if (read_all(data, n, &e) == 0) undetected++;
        data[i] = saved;
    }
    CHECK(undetected == 0, "%s: %d single-byte changes went undetected", name, undetected);
    for (i = 0; i < n; i++) {
        uint8_t *prefix = (uint8_t *)malloc(i ? i : 1);
        memcpy(prefix, data, i);
        CHECK(read_all(prefix, i, &e) != 0, "%s: a %zu-byte prefix was accepted", name, i);
        free(prefix);
    }
    free(data);
}

static void test_writer_errors(const char *tmpdir)
{
    rdz_error e;
    uint8_t big[RDZ_MAX_SYNOPSIS_LEN + 1];
    char path[512];
    memset(big, 0, sizeof big);
    snprintf(path, sizeof path, "%s/too-big.rdz", tmpdir);
    CHECK(rdz_write_generic(path, (const uint8_t *)"x", 1, big, sizeof big, &e) != 0 &&
              e.code == RDZ_E_LIMIT,
          "an oversized synopsis was written");
    CHECK(rdz_write_generic(tmpdir, (const uint8_t *)"x", 1, NULL, 0, &e) != 0 &&
              strstr(e.message, "destination is a directory") != NULL,
          "a directory was overwritten: %s", e.message);
    snprintf(path, sizeof path, "%s/no/such/dir/x.rdz", tmpdir);
    CHECK(rdz_write_generic(path, (const uint8_t *)"x", 1, NULL, 0, &e) != 0 &&
              e.code == RDZ_E_IO,
          "a file was written into a missing directory");
#ifndef _WIN32
    {
        struct stat st;
        FILE *f;
        snprintf(path, sizeof path, "%s/mode.rdz", tmpdir);
        f = fopen(path, "wb");
        if (f) fclose(f);
        chmod(path, 0640);
        CHECK(rdz_write_generic(path, (const uint8_t *)"abc", 3, NULL, 0, &e) == 0, "%s",
              e.message);
        CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640,
              "the destination's permissions were not kept");
        remove(path);
    }
#endif
}

/* Compresses n bytes in `block`-byte blocks through a pipeline of `threads`,
   writes them as a generic container, reads it back through a decode
   pipeline, and returns 0 when the bytes and the file agree with *expect
   (set on the first call). */
static int pipeline_roundtrip(const uint8_t *data, size_t n, uint32_t block, int threads,
                              const char *path, uint8_t **expect, size_t *expect_len)
{
    rdz_pipeline p;
    rdz_writer w;
    rdz_reader r;
    rdz_error e;
    rdz_slot *s;
    size_t at = 0, got_len = 0;
    uint8_t *got, *file;
    uint32_t i;
    int must, ok = 1;

    if (rdz_pipeline_init(&p, threads, rdz_job_compress, (size_t)RDZ_MAX_BLOCK_SIZE,
                          (size_t)RDZ_BLOCK_SIZE, &e) ||
        rdz_writer_open(&w, path, RDZ_CODEC_R_SERIAL_V3, RDZ_R_SERIAL_CODEC_VERSION, block, &e)) {
        CHECK(0, "pipeline setup: %s", e.message);
        return 1;
    }
    p.level = 1;
    while (at < n) {
        size_t take = n - at < block ? n - at : block;
        while ((s = rdz_pipeline_next(&p, &must)) != NULL && must) {
            const zb_buf *st = s->compression ? &s->out : &s->in;
            ok &= rdz_writer_stored(&w, 0, s->compression, s->decoded_len, s->decoded_len,
                                    st->data, st->len, s->checksum, &e) == 0;
            rdz_pipeline_release(&p, s);
        }
        zb_put_bytes(&s->in, data + at, take);
        rdz_pipeline_submit(&p, s, &e);
        at += take;
    }
    while ((s = rdz_pipeline_oldest(&p, 1)) != NULL) {
        const zb_buf *st = s->compression ? &s->out : &s->in;
        ok &= !s->failed && rdz_writer_stored(&w, 0, s->compression, s->decoded_len,
                                              s->decoded_len, st->data, st->len, s->checksum,
                                              &e) == 0;
        rdz_pipeline_release(&p, s);
    }
    rdz_pipeline_free(&p);
    ok &= rdz_writer_finish(&w, NULL, 0, NULL, 0, NULL, 0, &e) == 0;
    CHECK(ok, "pipeline write with %d threads: %s", threads, e.message);

    /* read back through a decode pipeline */
    got = (uint8_t *)malloc(n ? n : 1);
    if (rdz_reader_open(&r, path, &e) ||
        rdz_pipeline_init(&p, threads, rdz_job_decode, (size_t)RDZ_MAX_BLOCK_SIZE,
                          (size_t)RDZ_BLOCK_SIZE, &e)) {
        CHECK(0, "pipeline read setup: %s", e.message);
        free(got);
        return 1;
    }
    /* until every block is read and every submitted one consumed */
    for (i = 0; i < r.nblocks || p.next_consume != p.next_submit;) {
        while (i < r.nblocks && p.next_submit - p.next_consume < p.nslots) {
            s = rdz_pipeline_next(&p, &must);
            if (rdz_reader_read_stored(&r, i, &s->in, &e)) break;
            s->block = &r.blocks[i++];
            rdz_pipeline_submit(&p, s, &e);
        }
        s = rdz_pipeline_oldest(&p, 1);
        if (!s) break;
        if (s->failed) {
            CHECK(0, "pipeline read: %s", s->e.message);
        } else {
            const zb_buf *d = s->compression ? &s->out : &s->in;
            if (got_len + d->len <= n) memcpy(got + got_len, d->data, d->len);
            got_len += d->len;
        }
        rdz_pipeline_release(&p, s);
    }
    rdz_pipeline_free(&p);
    rdz_reader_close(&r);
    CHECK(got_len == n && memcmp(got, data, n) == 0, "pipeline read with %d threads differs",
          threads);
    free(got);

    {
        size_t m = 0;
        file = slurp(path, &m);
        if (!*expect) {
            *expect = file;
            *expect_len = m;
        } else {
            CHECK(file && m == *expect_len && memcmp(file, *expect, m) == 0,
                  "the file written with %d threads differs from one thread's", threads);
            free(file);
        }
    }
    remove(path);
    return 0;
}

/* ---- the native codec against the Rust writer's files ---- */

typedef struct {
    rdz_str *values; /* the names, pointing into `arena` */
    size_t n, cap;
    rdz_str *dict;
    size_t ndict, dcap;
    uint8_t *arena; /* copies of every string's bytes */
    size_t used, acap;
} names_buf;

static const uint8_t *keep_bytes(names_buf *b, const rdz_str *v)
{
    if (b->used + v->len + 1 > b->acap) {
        b->acap = (b->acap + v->len + 1) * 2;
        b->arena = (uint8_t *)realloc(b->arena, b->acap);
    }
    memcpy(b->arena + b->used, v->bytes, v->len);
    b->used += v->len;
    return (const uint8_t *)(uintptr_t)(b->used - v->len); /* an offset until fixed up */
}

static void push_value(rdz_str **arr, size_t *n, size_t *cap, rdz_str v)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        *arr = (rdz_str *)realloc(*arr, *cap * sizeof **arr);
    }
    (*arr)[(*n)++] = v;
}

static int sink_plain(void *ctx, const rdz_str *v, size_t count, rdz_error *e)
{
    names_buf *b = (names_buf *)ctx;
    size_t i;
    (void)e;
    for (i = 0; i < count; i++) {
        rdz_str c = v[i];
        c.bytes = keep_bytes(b, &v[i]);
        push_value(&b->values, &b->n, &b->cap, c);
    }
    return 0;
}

static int sink_entries(void *ctx, const rdz_str *v, size_t count, rdz_error *e)
{
    names_buf *b = (names_buf *)ctx;
    size_t i;
    (void)e;
    for (i = 0; i < count; i++) {
        rdz_str c = v[i];
        c.bytes = keep_bytes(b, &v[i]);
        push_value(&b->dict, &b->ndict, &b->dcap, c);
    }
    return 0;
}

static int sink_indices(void *ctx, const uint32_t *ids, size_t count, rdz_error *e)
{
    names_buf *b = (names_buf *)ctx;
    size_t i;
    (void)e;
    for (i = 0; i < count; i++) push_value(&b->values, &b->n, &b->cap, b->dict[ids[i]]);
    return 0;
}

/* Identity by content, as R's string cache gives it. */
static uintptr_t names_key(void *ctx, size_t i)
{
    const rdz_str *v = &((names_buf *)ctx)->values[i];
    uint64_t h = 1469598103934665603ull ^ v->tag;
    size_t k;
    for (k = 0; k < v->len; k++) h = (h ^ v->bytes[k]) * 1099511628211ull;
    return (uintptr_t)(h ^ ((uint64_t)v->len << 56));
}

static int names_value(void *ctx, size_t i, rdz_str *out, rdz_error *e)
{
    (void)e;
    *out = ((names_buf *)ctx)->values[i];
    return 0;
}

static void test_native_fixture(const char *name, const char *policy, const char *tmpdir)
{
    char path[512], copy[600];
    rdz_reader r;
    rdz_error e;
    size_t n = 0, len = 0, dict = 0, i, a = 0, b2 = 0;
    int present = 0, pol;
    int32_t *values;
    names_buf nb;
    rdz_names_sink sink;
    rdz_str_source src;
    uint8_t *original, *rewritten;

    memset(&nb, 0, sizeof nb);
    snprintf(path, sizeof path, FIXTURES "%s.rdz", name);
    if (rdz_reader_open(&r, path, &e) || rdz_native_length(&r, &n, &e)) {
        CHECK(0, "%s: %s", name, e.message);
        return;
    }
    values = (int32_t *)malloc((n ? n : 1) * sizeof *values);
    CHECK(rdz_native_read_logical(&r, values, &e) == 0, "%s: %s", name, e.message);
    rdz_native_names_info(&r, &present, &len, &dict, &e);
    sink.ctx = &nb;
    sink.plain = sink_plain;
    sink.entries = sink_entries;
    sink.indices = sink_indices;
    if (present) CHECK(rdz_native_read_names(&r, &sink, &e) == 0, "%s names: %s", name, e.message);
    rdz_reader_close(&r);
    /* fix up the arena offsets now that it has stopped moving */
    for (i = 0; i < nb.n; i++) nb.values[i].bytes = nb.arena + (uintptr_t)nb.values[i].bytes;
    CHECK(!present || nb.n == len, "%s: %zu names, expected %zu", name, nb.n, len);

    pol = strcmp(policy, "block") == 0 ? RDZ_DICT_BLOCK : strcmp(policy, "global") == 0
              ? RDZ_DICT_GLOBAL : strcmp(policy, "auto") == 0 ? RDZ_DICT_AUTO : RDZ_DICT_PLAIN;
    src.n = nb.n;
    src.ctx = &nb;
    src.key = names_key;
    src.value = names_value;
    snprintf(copy, sizeof copy, "%s/native-%s.rdz", tmpdir, name);
    if (rdz_write_native_logical(copy, values, n, present ? &src : NULL, pol, &e)) {
        CHECK(0, "%s: rewrite: %s", name, e.message);
    } else {
        original = slurp(path, &a);
        rewritten = slurp(copy, &b2);
        CHECK(same_but_writer(original, a, rewritten, b2),
              "%s: the C writer's native bytes differ from the Rust writer's", name);
        free(original);
        free(rewritten);
        remove(copy);
    }
    free(values);
    free(nb.values);
    free(nb.dict);
    free(nb.arena);
}

static void test_native_manifest(const char *tmpdir)
{
    FILE *f = fopen(FIXTURES "manifest.tsv", "r");
    char line[4096];
    int rows = 0, pass;
    if (!f) return;
    for (pass = 0; pass < 2; pass++) {
        rewind(f);
        if (!fgets(line, sizeof line, f)) break;
        rdz_logical_force_scalar(pass == 1);
        while (fgets(line, sizeof line, f)) {
            char *field[10];
            int k = 0;
            char *p = line;
            while (k < 10 && p) {
                field[k++] = p;
                p = strchr(p, '\t');
                if (p) *p++ = '\0';
            }
            if (k < 4 || strcmp(field[3], "native_v1") != 0) continue;
            test_native_fixture(field[0], field[2], tmpdir);
            rows++;
        }
    }
    rdz_logical_force_scalar(0);
    fclose(f);
    CHECK(rows == 2 * 21, "%d native fixture passes, expected 42", rows);
}

/* Integer and double vectors through the vector writer and reader, every
   record kind, at 1 and 4 threads, compressed and not; and every
   single-byte change of a small file is rejected or decodes in bounds. */
static void numeric_case(uint16_t type, const void *values, size_t n, int level, int threads,
                         const char *path, uint16_t want_encoding)
{
    rdz_vec v;
    rdz_reader r;
    rdz_error e;
    size_t size = type == RDZ_TYPE_DOUBLE ? 8 : 4;
    void *got = malloc(n * size + 8);
    uint32_t i;
    int seen = want_encoding == 0;
    rdz_vec_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.type = type;
    spec.n = n;
    spec.values = values;
    spec.level = level;
    spec.threads = threads;
    rdz_vec_init(&v);
    if (rdz_vec_write(&v, path, &spec, &e)) {
        CHECK(0, "vector write: %s", e.message);
        rdz_vec_free(&v);
        free(got);
        return;
    }
    rdz_vec_free(&v);
    rdz_vec_init(&v);
    if (rdz_reader_open(&r, path, &e) == 0) {
        for (i = 0; i < r.nblocks; i++) seen |= r.blocks[i].encoding == want_encoding;
        CHECK(rdz_vec_read(&v, &r, got, threads, NULL, NULL, &e) == 0, "vector read: %s", e.message);
        rdz_reader_close(&r);
        CHECK(memcmp(got, values, n * size) == 0, "type %u: values differ (level %d, %d threads)",
              type, level, threads);
    } else {
        CHECK(0, "vector open: %s", e.message);
    }
    CHECK(seen, "type %u: encoding %u never chosen", type, want_encoding);
    rdz_vec_free(&v);
    free(got);
    remove(path);
}

static void test_numeric(const char *tmpdir)
{
    size_t n = 600000, i;
    int32_t *iv = (int32_t *)malloc(n * sizeof *iv);
    double *dv = (double *)malloc(n * sizeof *dv);
    char path[512];
    uint32_t x = 7;
    int level, threads;
    snprintf(path, sizeof path, "%s/numeric.rdz", tmpdir);
    for (level = 0; level <= 1; level++) {
        for (threads = 1; threads <= 4; threads += 3) {
            for (i = 0; i < n; i++) iv[i] = (int32_t)(i * 3);
            numeric_case(RDZ_TYPE_INTEGER, iv, n, level, threads, path, 13);
            for (i = 0; i < n; i++) iv[i] = i % 5 == 0 ? INT32_MIN : (int32_t)(i % 100);
            numeric_case(RDZ_TYPE_INTEGER, iv, n, level, threads, path, 12);
            for (i = 0; i < n; i++) iv[i] = (int32_t)(i / 1000);
            numeric_case(RDZ_TYPE_INTEGER, iv, n, level, threads, path, 14);
            for (i = 0; i < n; i++) {
                x = x * 1103515245u + 12345u;
                iv[i] = (int32_t)(x | 1u) == INT32_MIN ? 1 : (int32_t)(x | 1u);
            }
            numeric_case(RDZ_TYPE_INTEGER, iv, n, level, threads, path, level ? 11 : 10);
            for (i = 0; i < n; i++) {
                x = x * 1103515245u + 12345u;
                dv[i] = (double)x / 3.0;
            }
            numeric_case(RDZ_TYPE_DOUBLE, dv, n, level, threads, path, level ? 21 : 20);
            for (i = 0; i < n; i++) dv[i] = i < n / 2 ? -0.0 : 2.5;
            numeric_case(RDZ_TYPE_DOUBLE, dv, n, level, threads, path, 22);
            numeric_case(RDZ_TYPE_DOUBLE, dv, 0, level, threads, path, 0);
        }
    }
    /* an empty block of no bytes: only raw and shuffled layouts hold nothing
       but values; the others have a header to read (never past `enc`) */
    {
        static const uint16_t ints[] = {10, 11, 12, 13, 14}, dbls[] = {20, 21, 22, 23};
        rdz_error e;
        size_t k;
        for (k = 0; k < sizeof ints / sizeof *ints; k++) {
            int refused = rdz_int_decode(NULL, 0, ints[k], 0, iv, &e) != 0;
            CHECK(refused == (ints[k] > RDZ_ENCODING_INT_SHUFFLE), "empty integer block, encoding %u",
                  (unsigned)ints[k]);
        }
        for (k = 0; k < sizeof dbls / sizeof *dbls; k++) {
            int refused = rdz_dbl_decode(NULL, 0, dbls[k], 0, dv, &e) != 0;
            CHECK(refused == (dbls[k] > RDZ_ENCODING_DBL_SHUFFLE), "empty double block, encoding %u",
                  (unsigned)dbls[k]);
        }
    }
    /* mutations of a small multi-encoding file */
    {
        uint8_t *data;
        size_t len = 0, k;
        rdz_vec v;
        rdz_error e;
        for (i = 0; i < 3000; i++) iv[i] = i < 1000 ? (int32_t)i : i < 2000 ? 7 : (int32_t)(i % 9);
        rdz_vec_spec spec;
        memset(&spec, 0, sizeof spec);
        spec.type = RDZ_TYPE_INTEGER;
        spec.n = 3000;
        spec.values = iv;
        spec.level = 1;
        spec.threads = 1;
        rdz_vec_init(&v);
        rdz_vec_write(&v, path, &spec, &e);
        rdz_vec_free(&v);
        data = slurp(path, &len);
        for (k = 0; data && k < len; k++) {
            rdz_reader r;
            data[k] ^= 0x5a;
            if (rdz_reader_open_memory(&r, data, len, &e) == 0) {
                rdz_vec_init(&v);
                CHECK(rdz_vec_read(&v, &r, dv, 1, NULL, NULL, &e) != 0,
                      "a change at byte %zu went undetected", k);
                rdz_vec_free(&v);
                rdz_reader_close(&r);
            }
            data[k] ^= 0x5a;
        }
        free(data);
        remove(path);
    }
    free(iv);
    free(dv);
}

/* ---- the decimal double codec (encoding 23, ALP) ------------------------------------ */

static double bits_double(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* encode and decode v[0, n) through the double codec, compressing; the
   encoding chosen, or 0 on a failure (reported) */
static uint16_t alp_roundtrip(const double *v, size_t n, const char *what)
{
    zb_buf out;
    uint16_t enc = 0;
    rdz_error e;
    double *got = (double *)malloc((n ? n : 1) * sizeof *got);
    zb_buf_alloc(&out, 0, 0);
    if (rdz_dbl_encode(v, n, 1, &out, &enc, &e)) {
        CHECK(0, "%s: encode: %s", what, e.message);
        enc = 0;
    } else if (rdz_dbl_decode(out.data, out.len, enc, n, got, &e)) {
        CHECK(0, "%s: decode (encoding %u): %s", what, enc, e.message);
        enc = 0;
    } else {
        CHECK(n == 0 || memcmp(got, v, n * 8) == 0, "%s: values differ (encoding %u)", what, enc);
        CHECK(enc != RDZ_ENCODING_DBL_DECIMAL || out.len < 8 * n,
              "%s: a decimal record no smaller than raw", what);
    }
    zb_buf_release(&out);
    free(got);
    return enc;
}

/* a hand-made decimal record, decoded: whether it was refused */
static int alp_refused(const uint8_t *rec, size_t len, size_t n)
{
    double *x = (double *)malloc(n * sizeof *x);
    rdz_error e;
    int refused = rdz_dbl_decode(rec, len, RDZ_ENCODING_DBL_DECIMAL, n, x, &e) != 0;
    free(x);
    return refused;
}

static void test_alp(const char *tmpdir)
{
    static const size_t lengths[] = {64, 100, 1023, 1024, 1025, 3000, 131072};
    size_t big = 131072, i, li;
    double *v = (double *)malloc(big * sizeof *v);
    uint32_t x = 99;
    char path[512];
    const double specials[] = {0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 4.9e-324, 1e300, -1e-300,
                               9007199254740992.0, 0.1, 123456.789};
    snprintf(path, sizeof path, "%s/alp.rdz", tmpdir);

    /* the format's premise: with |m| < 2^53, m / 10^e in double arithmetic is
       the double strtod() reads "me-e" as (one correctly rounded division) */
    {
        static const double p10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
                                     1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
        int mismatches = 0, k;
        uint64_t s = 12345;
        for (k = 0; k < 100000; k++) {
            long long m;
            int p;
            char buf[48];
            volatile double q;
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            m = (long long)((s >> 11) & ((1ull << 53) - 1)) >> (int)((s >> 3) % 50);
            if (s & 1) m = -m;
            p = (int)((s >> 58) % 19);
            q = (double)m / p10[p];
            snprintf(buf, sizeof buf, "%llde-%d", m, p);
            if (q != strtod(buf, NULL)) mismatches++;
        }
#if !(defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0)
        CHECK(mismatches == 0, "%d of 100,000 divisions differ from strtod()", mismatches);
#else
        printf("x87: %d of 100,000 divisions differ from strtod() (why ALP decodes with it)\n",
               mismatches);
#endif
    }

    for (li = 0; li < sizeof lengths / sizeof lengths[0]; li++) {
        size_t n = lengths[li];
        int places;
        for (places = 0; places <= 6; places++) {
            double scale = 1;
            int k;
            char what[64];
            for (k = 0; k < places; k++) scale *= 10;
            for (i = 0; i < n; i++) {
                x = x * 1103515245u + 12345u;
                /* as R reads a decimal: the nearest double to m / 10^places */
                v[i] = (double)(int32_t)(x >> 4) / scale;
            }
            snprintf(what, sizeof what, "%zu decimals with %d places", n, places);
            CHECK(alp_roundtrip(v, n, what) == RDZ_ENCODING_DBL_DECIMAL, "%s: not decimal", what);
            /* with R's NA, NaN payloads, -0, infinities and others among them */
            for (i = 0; i < n; i += 37) {
                v[i] = i % 3 == 0 ? bits_double(0x7FF00000000007A2ull)
                       : i % 3 == 1 ? bits_double(0x7FF8000000000123ull)
                                    : specials[(i / 37) % 10];
            }
            snprintf(what, sizeof what, "%zu decimals with %d places and exceptions", n, places);
            alp_roundtrip(v, n, what);
        }
        /* thousands (a factor f), a random walk (delta), a constant */
        for (i = 0; i < n; i++) v[i] = (double)((i * 7919) % 1000) * 1000.0;
        alp_roundtrip(v, n, "multiples of 1000");
        for (i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            v[i] = (i ? v[i - 1] * 100 + (double)((int)(x >> 28) - 8) : 1000000.0) / 100;
            v[i] = (double)(long long)(v[i] * 100 + (v[i] < 0 ? -0.5 : 0.5)) / 100;
        }
        alp_roundtrip(v, n, "a random walk in cents");
        for (i = 0; i < n; i++) v[i] = 2.75;
        {
            uint16_t c = alp_roundtrip(v, n, "a constant");
            /* runs, or a decimal record when that is smaller (one short vector) */
            CHECK(c == RDZ_ENCODING_DBL_RUNS || c == RDZ_ENCODING_DBL_DECIMAL,
                  "a constant is encoding %u", c);
        }
    }
    /* full-precision values stay shuffled; small blocks stay raw */
    for (i = 0; i < big; i++) {
        x = x * 1103515245u + 12345u;
        v[i] = (double)x / 3.0;
    }
    CHECK(alp_roundtrip(v, big, "thirds") == RDZ_ENCODING_DBL_SHUFFLE, "thirds are not decimals");
    for (i = 0; i < 10; i++) v[i] = (double)i / 10;
    CHECK(alp_roundtrip(v, 10, "ten tenths") != RDZ_ENCODING_DBL_DECIMAL, "ten values are not ALP");

    /* through files: chosen when compressing, never at the speed preset */
    {
        double *d = (double *)malloc(300000 * sizeof *d);
        for (i = 0; i < 300000; i++) d[i] = (double)(long long)((i * 2654435761u) % 100000) / 100;
        numeric_case(RDZ_TYPE_DOUBLE, d, 300000, 1, 1, path, RDZ_ENCODING_DBL_DECIMAL);
        numeric_case(RDZ_TYPE_DOUBLE, d, 300000, 1, 4, path, RDZ_ENCODING_DBL_DECIMAL);
        numeric_case(RDZ_TYPE_DOUBLE, d, 300000, 0, 1, path, RDZ_ENCODING_DBL_RAW);
        /* every single-byte change to a small decimal file is caught */
        {
            rdz_vec vv;
            rdz_vec_spec spec;
            rdz_error e;
            uint8_t *data;
            size_t len = 0, k;
            memset(&spec, 0, sizeof spec);
            spec.type = RDZ_TYPE_DOUBLE;
            spec.n = 2000;
            spec.values = d;
            spec.level = 1;
            spec.threads = 1;
            rdz_vec_init(&vv);
            rdz_vec_write(&vv, path, &spec, &e);
            rdz_vec_free(&vv);
            data = slurp(path, &len);
            for (k = 0; data && k < len; k++) {
                rdz_reader r;
                data[k] ^= 0x5a;
                if (rdz_reader_open_memory(&r, data, len, &e) == 0) {
                    rdz_vec_init(&vv);
                    CHECK(rdz_vec_read(&vv, &r, v, 1, NULL, NULL, &e) != 0,
                          "decimal file: a change at byte %zu went undetected", k);
                    rdz_vec_free(&vv);
                    rdz_reader_close(&r);
                }
                data[k] ^= 0x5a;
            }
            free(data);
            remove(path);
        }
        free(d);
    }

    /* hand-made records: one vector of 100 values, all 5 (width 0) */
    {
        uint8_t rec[64];
        memset(rec, 0, sizeof rec);
        rec[0] = 0; rec[1] = 0; rec[2] = 0; rec[3] = 0;
        zb_wr_u64le(rec + 8, 5);
        CHECK(!alp_refused(rec, 16, 100), "a minimal decimal record was refused");
        rec[0] = 19;
        CHECK(alp_refused(rec, 16, 100), "an exponent past 18 was accepted");
        rec[0] = 2; rec[1] = 3;
        CHECK(alp_refused(rec, 16, 100), "a factor past the exponent was accepted");
        rec[1] = 0; rec[3] = 2;
        CHECK(alp_refused(rec, 16, 100), "unknown vector flags were accepted");
        rec[3] = 0; rec[6] = 1;
        CHECK(alp_refused(rec, 16, 100), "a nonzero reserved field was accepted");
        rec[6] = 0;
        CHECK(alp_refused(rec, 17, 100), "trailing bytes were accepted");
        /* an exception past the vector, and two out of order */
        zb_wr_u16le(rec + 4, 1);
        zb_wr_u16le(rec + 16, 100);
        CHECK(alp_refused(rec, 26, 100), "an exception past the vector was accepted");
        zb_wr_u16le(rec + 4, 2);
        zb_wr_u16le(rec + 16, 7);
        zb_wr_u16le(rec + 18, 7);
        CHECK(alp_refused(rec, 36, 100), "repeated exception positions were accepted");
        /* a value too large for an exact decode */
        memset(rec, 0, sizeof rec);
        rec[1] = 0;
        zb_wr_u64le(rec + 8, 9007199254740992ull);
        CHECK(alp_refused(rec, 16, 100), "a value of 2^53 was accepted");
        /* width 65 */
        memset(rec, 0, sizeof rec);
        rec[2] = 65;
        CHECK(alp_refused(rec, 16, 1), "a width past 64 was accepted");
        /* nonzero padding: 3 values of 3 bits */
        memset(rec, 0, sizeof rec);
        rec[2] = 3;
        rec[16] = 0;
        rec[17] = 0x80;
        CHECK(alp_refused(rec, 18, 3), "nonzero padding bits were accepted");
    }
    free(v);
}

static void test_pipeline(const char *tmpdir)
{
    size_t n = 3 * 1024 * 1024, i;
    uint8_t *data = (uint8_t *)malloc(n), *expect = NULL;
    size_t expect_len = 0;
    uint32_t x = 12345;
    char path[512];
    int threads[] = {1, 2, 8};
    int k;
    /* read-ahead is bounded in bytes: 256 threads of 64 MiB blocks get 16
       slots (1 GiB), of 1 MiB blocks their 512 */
    {
        rdz_pipeline p;
        rdz_error e;
        CHECK(!rdz_pipeline_init(&p, 256, rdz_job_decode, (size_t)RDZ_MAX_BLOCK_SIZE,
                                 (size_t)RDZ_MAX_BLOCK_SIZE, &e) && p.nslots == 16,
              "64 MiB blocks: %u slots", (unsigned)p.nslots);
        rdz_pipeline_free(&p);
        CHECK(!rdz_pipeline_init(&p, 256, rdz_job_decode, (size_t)RDZ_MAX_BLOCK_SIZE,
                                 (size_t)RDZ_BLOCK_SIZE, &e) && p.nslots == 512,
              "1 MiB blocks: %u slots", (unsigned)p.nslots);
        rdz_pipeline_free(&p);
    }
    /* compressible stretches and noise, so blocks of both kinds occur */
    for (i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (i / 65536) % 2 ? (uint8_t)(x >> 24) : (uint8_t)(i % 7);
    }
    snprintf(path, sizeof path, "%s/pipeline.rdz", tmpdir);
    for (k = 0; k < 3; k++) {
        pipeline_roundtrip(data, n, 4096, threads[k], path, &expect, &expect_len);
    }
    {
        rdz_reader r;
        rdz_error e;
        uint32_t raw = 0, zstd = 0;
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(expect, 1, expect_len, f);
            fclose(f);
        }
        if (rdz_reader_open(&r, path, &e) == 0) {
            for (i = 0; i < r.nblocks; i++) {
                if (r.blocks[i].compression) zstd++;
                else raw++;
            }
            rdz_reader_close(&r);
        }
        CHECK(raw > 0 && zstd > 0, "expected raw and zstd blocks, got %u and %u", raw, zstd);
        remove(path);
    }
    free(expect);
    free(data);
}

/* ---- what a later writer may add, which 0.1.0 readers accept ---------------------- */

static void *ext_values(void *ctx, uint32_t object) { (void)object; return ctx; }

/* Opens and reads an integer root of up to 8 values into out. */
static int ext_read(const uint8_t *data, size_t n, int32_t *out, rdz_error *e)
{
    rdz_reader r;
    rdz_graph_sinks sinks;
    rdz_vec v;
    int failed;
    if (rdz_reader_open_memory(&r, data, n, e)) return 1;
    sinks.ctx = out;
    sinks.values = ext_values;
    sinks.strings = NULL;
    rdz_vec_init(&v);
    failed = r.nobjects != 1 || r.objects[0].logical_len > 8 ||
             rdz_graph_read(&v, &r, &sinks, 1, NULL, NULL, e);
    rdz_vec_free(&v);
    rdz_reader_close(&r);
    return failed;
}

/* c(1:6) as raw integer blocks of the given sizes. */
static uint8_t *ext_file(const char *path, const uint32_t *sizes, uint32_t nsizes, size_t *n)
{
    rdz_writer w;
    rdz_object o;
    rdz_error e;
    uint8_t raw[24];
    uint32_t i, at = 0;
    for (i = 0; i < 6; i++) zb_wr_u32le(raw + 4 * i, i + 1);
    memset(&o, 0, sizeof o);
    o.parent_id = RDZ_ROOT_PARENT_ID;
    o.type_tag = RDZ_TYPE_INTEGER;
    o.logical_len = 6;
    o.block_count = nsizes;
    if (rdz_writer_open(&w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION, RDZ_BLOCK_SIZE,
                        &e)) {
        return NULL;
    }
    for (i = 0; i < nsizes; i++) {
        if (rdz_writer_block(&w, RDZ_ENCODING_INT_RAW, sizes[i], raw + 4 * at, 4 * sizes[i], &e)) {
            rdz_writer_discard(&w);
            return NULL;
        }
        at += sizes[i];
    }
    if (rdz_writer_finish(&w, &o, 1, NULL, 0, NULL, 0, &e)) {
        rdz_writer_discard(&w);
        return NULL;
    }
    return slurp(path, n);
}

/* An empty logical root in one block of `encoding` holding `bytes`: only
   the canonical encoding-4 record (its 4-byte header) is accepted. */
static void test_empty_logical(const char *tmpdir)
{
    static const uint8_t header[RDZ_LOGICAL_DENSE_HEADER_LEN] = {0};
    char path[512];
    int k;
    snprintf(path, sizeof path, "%s/empty-logical.rdz", tmpdir);
    for (k = 0; k < 2; k++) {
        uint16_t encoding = k ? RDZ_ENCODING_LOGICAL_DENSE_PLANES : RDZ_ENCODING_LOGICAL_2BIT;
        rdz_writer w;
        rdz_reader r;
        rdz_object o;
        rdz_error e;
        uint8_t *data;
        size_t n = 0;
        int opened;
        memset(&o, 0, sizeof o);
        o.parent_id = RDZ_ROOT_PARENT_ID;
        o.type_tag = RDZ_TYPE_LOGICAL;
        o.block_count = 1;
        if (rdz_writer_open(&w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION,
                            RDZ_BLOCK_SIZE, &e) ||
            rdz_writer_block(&w, encoding, 0, header, k ? sizeof header : 0, &e) ||
            rdz_writer_finish(&w, &o, 1, NULL, 0, NULL, 0, &e)) {
            CHECK(0, "empty logical, encoding %u: %s", (unsigned)encoding, e.message);
            continue;
        }
        data = slurp(path, &n);
        opened = data && rdz_reader_open_memory(&r, data, n, &e) == 0;
        if (opened) rdz_reader_close(&r);
        CHECK(opened == (k == 1), "empty logical in encoding %u: opened %d (%s)",
              (unsigned)encoding, opened, opened ? "" : e.message);
        free(data);
    }
}

static void ext_reseal(uint8_t *data, size_t n)
{
    size_t dir = (size_t)zb_rd_u64le(data + n - RDZ_TRAILER_LEN + 8);
    zb_wr_u64le(data + 24, rdz_hash(data, 24));
    zb_wr_u64le(data + dir + 32, rdz_hash(data + dir, 32));
    zb_wr_u64le(data + n - RDZ_TRAILER_LEN + 24, rdz_hash(data + dir, n - RDZ_TRAILER_LEN - dir));
}

/* The file with `pad` zero bytes after the directory header and after every
   entry, as a later writer's wider records would be. */
static uint8_t *ext_widen(const uint8_t *data, size_t n, size_t pad, size_t *out_n)
{
    size_t dir = (size_t)zb_rd_u64le(data + n - RDZ_TRAILER_LEN + 8), at, from, i;
    uint32_t nobj = zb_rd_u32le(data + dir + 16), natt = zb_rd_u32le(data + dir + 20),
             nblk = zb_rd_u32le(data + dir + 24);
    size_t widths[3] = {RDZ_OBJECT_ENTRY_LEN, RDZ_ATTRIBUTE_ENTRY_LEN, RDZ_BLOCK_ENTRY_LEN};
    uint32_t counts[3];
    size_t len = n + pad * (1 + (size_t)nobj + natt + nblk);
    uint8_t *w = (uint8_t *)calloc(len, 1);
    int k;
    counts[0] = nobj;
    counts[1] = natt;
    counts[2] = nblk;
    if (!w) return NULL;
    memcpy(w, data, dir + RDZ_DIRECTORY_HEADER_LEN);
    zb_wr_u16le(w + dir + 6, (uint16_t)(RDZ_DIRECTORY_HEADER_LEN + pad));
    zb_wr_u16le(w + dir + 8, (uint16_t)(RDZ_OBJECT_ENTRY_LEN + pad));
    zb_wr_u16le(w + dir + 10, (uint16_t)(RDZ_ATTRIBUTE_ENTRY_LEN + pad));
    zb_wr_u16le(w + dir + 12, (uint16_t)(RDZ_BLOCK_ENTRY_LEN + pad));
    at = dir + RDZ_DIRECTORY_HEADER_LEN + pad;
    from = dir + RDZ_DIRECTORY_HEADER_LEN;
    for (k = 0; k < 3; k++) {
        for (i = 0; i < counts[k]; i++) {
            memcpy(w + at, data + from, widths[k]);
            memset(w + at + widths[k], 0xa5, pad);
            at += widths[k] + pad;
            from += widths[k];
        }
    }
    memcpy(w + at, data + from, n - from); /* the synopsis (none) and trailer */
    zb_wr_u64le(w + len - RDZ_TRAILER_LEN + 16, (uint64_t)(len - RDZ_TRAILER_LEN - dir));
    ext_reseal(w, len);
    *out_n = len;
    return w;
}

static void test_extensions(const char *tmpdir)
{
    static const uint32_t even[1] = {6}, uneven[3] = {2, 3, 1}, empty_block[2] = {6, 0};
    char path[512];
    size_t n, m, k;
    uint8_t *data, *wide;
    int32_t out[8];
    rdz_error e;
    snprintf(path, sizeof path, "%s/ext.rdz", tmpdir);

    /* any block size up to the maximum: sizes are the writer's policy */
    data = ext_file(path, uneven, 3, &n);
    CHECK(data && ext_read(data, n, out, &e) == 0 && out[0] == 1 && out[5] == 6,
          "blocks of 2, 3 and 1 values: %s", e.message);
    free(data);
    data = ext_file(path, empty_block, 2, &n);
    CHECK(data && ext_read(data, n, out, &e) != 0, "an empty block in a nonempty object was read");
    free(data);

    data = ext_file(path, even, 1, &n);
    CHECK(data && ext_read(data, n, out, &e) == 0, "the base file: %s", e.message);
    if (!data) return;

    /* the ignorable halves of the flags words; the required halves are
       still refused */
    {
        size_t dir = (size_t)zb_rd_u64le(data + n - RDZ_TRAILER_LEN + 8);
        size_t object = dir + RDZ_DIRECTORY_HEADER_LEN, block = object + RDZ_OBJECT_ENTRY_LEN;
        struct { size_t at; int width; uint32_t bits; int ok; } cases[] = {
            {RDZ_FH_FLAGS, 4, 0x00010000u, 1}, {RDZ_FH_FLAGS, 4, 0x00000001u, 0},
            {0, 2, 0x0100u, 1}, {0, 2, 0x0001u, 0},
            {0, 4, 0x80000000u, 1}, {0, 4, 0x00000002u, 0}};
        cases[2].at = cases[3].at = dir + 14;
        cases[4].at = cases[5].at = object + 12;
        for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            uint8_t *c = (uint8_t *)malloc(n);
            memcpy(c, data, n);
            if (cases[k].width == 4) {
                zb_wr_u32le(c + cases[k].at, zb_rd_u32le(c + cases[k].at) | cases[k].bits);
            } else {
                zb_wr_u16le(c + cases[k].at, (uint16_t)(zb_rd_u16le(c + cases[k].at) | cases[k].bits));
            }
            ext_reseal(c, n);
            CHECK((ext_read(c, n, out, &e) == 0) == cases[k].ok, "flags case %zu: %s", k,
                  cases[k].ok ? e.message : "accepted");
            free(c);
        }
        /* a block's ignorable flags, repeated in its header */
        {
            uint8_t *c = (uint8_t *)malloc(n);
            memcpy(c, data, n);
            zb_wr_u16le(c + RDZ_HEADER_LEN + RDZ_BH_FLAGS, 0x0100u);
            CHECK(ext_read(c, n, out, &e) != 0, "a block header disagreeing with its entry was read");
            zb_wr_u16le(c + RDZ_HEADER_LEN + RDZ_BH_FLAGS, 0);
            zb_wr_u32le(c + block + 4, 0x40000000u); /* beyond the header's 16 bits: ignored */
            ext_reseal(c, n);
            CHECK(ext_read(c, n, out, &e) == 0, "ignorable block flags: %s", e.message);
            free(c);
        }
    }

    /* wider directory records, their extra bytes skipped */
    wide = ext_widen(data, n, 16, &m);
    CHECK(wide && ext_read(wide, m, out, &e) == 0 && out[5] == 6, "wider records: %s",
          e.message);
    free(wide);
    wide = ext_widen(data, n, RDZ_MAX_ENTRY_WIDTH, &m);
    CHECK(wide && ext_read(wide, m, out, &e) != 0, "entries wider than the maximum were read");
    free(wide);
    free(data);
    remove(path);
}

int main(int argc, char **argv)
{
    const char *tmpdir = argc > 1 ? argv[1] : ".";
    test_records();
    test_manifest(tmpdir);
    test_mutations(FIXTURES, "gen_integer_auto");
    test_mutations(FIXTURES, "lgl_names_encodings_global");
    {
        /* every flipped byte and every prefix of frozen files that cover the
           graph: frames, row names, nested lists, dictionaries, shared
           vectors, decimals, metadata and empty vectors */
        static const char *const frozen[] = {
            "metadata_balanced", "chr_dictionary_balanced", "frame_classed_balanced",
            "frame_row_names_balanced", "nested_list_balanced", "empty_vectors_balanced",
            "shared_balanced", "dbl_decimal_balanced", "attributes_balanced"};
        size_t k;
        for (k = 0; k < sizeof frozen / sizeof *frozen; k++) {
            test_mutations("tests/testthat/fixtures/v0.1.0/", frozen[k]);
        }
    }
    test_writer_errors(tmpdir);
    test_pipeline(tmpdir);
    test_native_manifest(tmpdir);
    test_numeric(tmpdir);
    test_alp(tmpdir);
    test_extensions(tmpdir);
    test_empty_logical(tmpdir);
    printf("logical kernel: %s\n", rdz_logical_kernel());
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
