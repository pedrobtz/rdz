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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "rdz_container.h"

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
            CHECK(original && rewritten && n == m && memcmp(original, rewritten, n) == 0,
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

static void test_mutations(const char *name)
{
    char path[512];
    size_t n = 0, i;
    uint8_t *data;
    rdz_error e;
    int undetected = 0;
    snprintf(path, sizeof path, FIXTURES "%s.rdz", name);
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

int main(int argc, char **argv)
{
    const char *tmpdir = argc > 1 ? argv[1] : ".";
    test_records();
    test_manifest(tmpdir);
    test_mutations("gen_integer_auto");
    test_mutations("lgl_names_encodings_global");
    test_writer_errors(tmpdir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
