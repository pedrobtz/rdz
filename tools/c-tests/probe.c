/*
 * probe.c -- one hostile file per reader guard, for tools/run-mutation-check
 * (plan-c Stage I). `probe DIR CASE` writes the case's file into DIR through
 * the container writer (so every checksum is valid unless the case breaks
 * one on purpose), reads it as the fuzz target does, every block and the
 * whole object graph, and prints "OK" or the reader's message. `probe DIR`
 * lists the cases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zubin/rw.h>

#include "rdz_container.h"
#include "rdz_graph.h"
#include "../../src/vendor/zstd/zstd.h"

#define MAX_BLOCKS 4

typedef struct {
    uint16_t encoding, compression;
    uint64_t count, decoded;
    uint8_t bytes[512];
    size_t n;
} blk;

typedef struct {
    blk blocks[MAX_BLOCKS];
    uint32_t nblocks;
    rdz_object *objects;
    uint32_t nobjects;
    rdz_attribute attributes[2];
    uint32_t nattributes;
    uint32_t block_size;
} spec;

static rdz_object object(uint32_t id, uint32_t parent, uint16_t role, uint16_t type,
                         uint64_t len, uint32_t first_block, uint32_t block_count)
{
    rdz_object o;
    memset(&o, 0, sizeof o);
    o.object_id = id;
    o.parent_id = parent;
    o.role = role;
    o.type_tag = type;
    o.logical_len = len;
    o.first_block = first_block;
    o.block_count = block_count;
    return o;
}

static void add(spec *s, uint16_t encoding, uint64_t count, const void *bytes, size_t n)
{
    blk *b = &s->blocks[s->nblocks++];
    b->encoding = encoding;
    b->compression = RDZ_COMPRESSION_NONE;
    b->count = count;
    b->decoded = n;
    memcpy(b->bytes, bytes, n);
    b->n = n;
}

/* A block stored as one zstd frame of `raw`, declaring `decoded` bytes. */
static void add_zstd(spec *s, uint16_t encoding, uint64_t count, const void *raw, size_t n,
                     uint64_t decoded)
{
    blk *b = &s->blocks[s->nblocks++];
    b->encoding = encoding;
    b->compression = RDZ_COMPRESSION_ZSTD;
    b->count = count;
    b->decoded = decoded;
    b->n = ZSTD_compress(b->bytes, sizeof b->bytes, raw, n, 1);
}

static int write_spec(const char *path, const spec *s, rdz_error *e)
{
    rdz_writer w;
    uint32_t i;
    if (rdz_writer_open(&w, path, RDZ_CODEC_NATIVE_V1, RDZ_NATIVE_CODEC_VERSION,
                        s->block_size ? s->block_size : RDZ_BLOCK_SIZE, e)) {
        return 1;
    }
    for (i = 0; i < s->nblocks; i++) {
        const blk *b = &s->blocks[i];
        if (rdz_writer_stored(&w, b->encoding, b->compression, b->count, b->decoded, b->bytes,
                              b->n, rdz_hash(b->bytes, b->n), e)) {
            rdz_writer_discard(&w);
            return 1;
        }
    }
    if (rdz_writer_finish(&w, s->objects, s->nobjects, s->attributes, s->nattributes, NULL, 0,
                          e)) {
        rdz_writer_discard(&w);
        return 1;
    }
    return 0;
}

/* ---- reading, as the fuzz target does --------------------------------------------- */

static void *values_of(void *ctx, uint32_t object_id)
{
    (void)object_id;
    return ctx;
}

static int ignore_strings(void *ctx, const rdz_str *v, size_t n, rdz_error *e)
{
    (void)ctx; (void)v; (void)n; (void)e;
    return 0;
}

static int ignore_ids(void *ctx, const uint32_t *ids, size_t n, rdz_error *e)
{
    (void)ctx; (void)ids; (void)n; (void)e;
    return 0;
}

static rdz_names_sink ignore = {NULL, ignore_strings, ignore_strings, ignore_ids};

static const rdz_names_sink *strings_of(void *ctx, uint32_t object_id)
{
    (void)ctx; (void)object_id;
    return &ignore;
}

static int read_all(const uint8_t *data, size_t n, rdz_error *e)
{
    rdz_reader r;
    zb_buf block;
    uint32_t i, k;
    size_t most = 1;
    int failed = 0;
    if (rdz_reader_open_memory(&r, data, n, e)) return 1;
    if (zb_buf_alloc(&block, 0, (size_t)RDZ_MAX_BLOCK_SIZE)) {
        rdz_reader_close(&r);
        return rdz_memory(e, "a block");
    }
    for (i = 0; i < r.nblocks && !failed; i++) failed = rdz_reader_read_block(&r, i, &block, e);
    zb_buf_release(&block);
    /* every object decodes into one array as long as the longest, so an
       overrun is an overrun of exactly that */
    for (k = 0; k < r.nobjects; k++) {
        if (r.objects[k].logical_len > most) most = (size_t)r.objects[k].logical_len;
    }
    if (!failed && r.codec_id == RDZ_CODEC_NATIVE_V1 && most <= ((size_t)1 << 20)) {
        double *values = (double *)malloc(most * sizeof(double));
        rdz_graph_sinks sinks;
        rdz_vec v;
        sinks.ctx = values;
        sinks.values = values_of;
        sinks.strings = strings_of;
        rdz_vec_init(&v);
        failed = rdz_graph_read(&v, &r, &sinks, 1, NULL, NULL, e);
        rdz_vec_free(&v);
        free(values);
    }
    rdz_reader_close(&r);
    return failed;
}

/* ---- patches applied after writing ------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t n;
    size_t directory;
} file;

static void reseal_header(file *f)
{
    zb_wr_u64le(f->data + 24, rdz_hash(f->data, 24));
}

static void reseal_directory_header(file *f)
{
    zb_wr_u64le(f->data + f->directory + 32, rdz_hash(f->data + f->directory, 32));
}

static void reseal_directory(file *f)
{
    size_t trailer = f->n - RDZ_TRAILER_LEN;
    zb_wr_u64le(f->data + trailer + 24,
                rdz_hash(f->data + f->directory, trailer - f->directory));
}

static uint8_t *object_entry(file *f, uint32_t i)
{
    return f->data + f->directory + RDZ_DIRECTORY_HEADER_LEN + (size_t)i * RDZ_OBJECT_ENTRY_LEN;
}

static uint8_t *block_entry(file *f, uint32_t nobjects, uint32_t nattributes, uint32_t i)
{
    return f->data + f->directory + RDZ_DIRECTORY_HEADER_LEN +
           (size_t)nobjects * RDZ_OBJECT_ENTRY_LEN + (size_t)nattributes * RDZ_ATTRIBUTE_ENTRY_LEN +
           (size_t)i * RDZ_BLOCK_ENTRY_LEN;
}

/* ---- the cases ---------------------------------------------------------------------- */

static const uint8_t ints3[12] = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0};

/* An integer root c(1L, 2L, 3L) in one raw block. */
static void int_root(spec *s, rdz_object *o)
{
    o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_INTEGER, 3, 0, 1);
    add(s, 10, 3, ints3, sizeof ints3);
    s->objects = o;
    s->nobjects = 1;
}

static const char *const cases[] = {
    "header-checksum", "directory-checksum", "block-checksum", "object-count",
    "directory-length", "block-offsets", "compressed-smaller", "declared-block-size",
    "block-range", "numeric-length", "logical-length", "string-length", "children-range",
    "depth", "frame-rows", "attribute-range", "delta-width", "int-run-end", "logical-run-end",
    "string-record-length", "dict-index-range", "zstd-length", "alp-exception-position", NULL};

static int is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int run(const char *dir, const char *name)
{
    char path[1024];
    spec s;
    rdz_object o[8], *deep = NULL;
    rdz_error e;
    file f;
    FILE *in;
    long len;
    int failed;

    memset(&s, 0, sizeof s);
    memset(o, 0, sizeof o);
    snprintf(path, sizeof path, "%s/%s.rdz", dir, name);

    if (is(name, "header-checksum") || is(name, "directory-checksum") ||
        is(name, "block-checksum") || is(name, "object-count") ||
        is(name, "directory-length") || is(name, "block-offsets") ||
        is(name, "declared-block-size")) {
        int_root(&s, o);
    } else if (is(name, "compressed-smaller")) {
        /* two integers: a zstd frame of 8 bytes is longer than 8 bytes */
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_INTEGER, 2, 0, 1);
        add_zstd(&s, 10, 2, ints3, 8, 8);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "block-range")) {
        int_root(&s, o);
        o[0].block_count = 3;
    } else if (is(name, "numeric-length")) {
        int_root(&s, o);
        o[0].logical_len = 4;
    } else if (is(name, "logical-length")) {
        static const uint8_t constant[4] = {1, 0, 0, 0};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_LOGICAL, 5, 0, 1);
        add(&s, RDZ_ENCODING_LOGICAL_CONSTANT, 4, constant, sizeof constant);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "string-length")) {
        static const uint8_t a[6] = {2, 1, 0, 0, 0, 'a'};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_CHARACTER, 2, 0, 1);
        add(&s, RDZ_ENCODING_STRING_PLAIN, 1, a, sizeof a);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "children-range")) {
        /* a list claiming two elements when the file has one */
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_LIST, 2, 0, 0);
        o[0].first_child = 1;
        o[0].child_count = 2;
        o[1] = object(1, 0, RDZ_ROLE_CHILD, RDZ_TYPE_INTEGER, 3, 0, 1);
        add(&s, 10, 3, ints3, sizeof ints3);
        s.objects = o;
        s.nobjects = 2;
    } else if (is(name, "depth")) {
        /* lists nested one level deeper than the limit, around a NULL */
        uint32_t i, n = RDZ_MAX_DEPTH + 2;
        deep = (rdz_object *)calloc(n, sizeof *deep);
        if (!deep) return 2;
        for (i = 0; i < n; i++) {
            int last = i + 1 == n;
            deep[i] = object(i, i ? i - 1 : RDZ_ROOT_PARENT_ID, i ? RDZ_ROLE_CHILD : RDZ_ROLE_ROOT,
                             last ? RDZ_TYPE_NULL : RDZ_TYPE_LIST, last ? 0 : 1, 0, 0);
            if (!last) {
                deep[i].first_child = i + 1;
                deep[i].child_count = 1;
            }
        }
        s.objects = deep;
        s.nobjects = n;
    } else if (is(name, "frame-rows")) {
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_DATA_FRAME, 2, 0, 0);
        o[0].first_child = 1;
        o[0].child_count = 1;
        o[1] = object(1, 0, RDZ_ROLE_CHILD, RDZ_TYPE_INTEGER, 3, 0, 1);
        add(&s, 10, 3, ints3, sizeof ints3);
        s.objects = o;
        s.nobjects = 2;
    } else if (is(name, "attribute-range")) {
        /* names whose value object does not exist */
        static const uint8_t nm[10] = {2, 5, 0, 0, 0, 'n', 'a', 'm', 'e', 's'};
        int_root(&s, o);
        o[0].attribute_count = 1;
        o[1] = object(1, 0, RDZ_ROLE_ATTRIBUTE_NAME, RDZ_TYPE_CHARACTER, 1, 1, 1);
        add(&s, RDZ_ENCODING_STRING_PLAIN, 1, nm, sizeof nm);
        s.nobjects = 2;
        s.attributes[0].owner_id = 0;
        s.attributes[0].name_object_id = 1;
        s.attributes[0].value_object_id = 50;
        s.attributes[0].flags = RDZ_ATTRIBUTE_FLAG_NAMES;
        s.nattributes = 1;
    } else if (is(name, "delta-width")) {
        /* width 33: c(5L, 5L) with a 33-bit zero code */
        uint8_t d[21] = {33, 0, 0, 0, 5, 0, 0, 0};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_INTEGER, 2, 0, 1);
        add(&s, 13, 2, d, sizeof d);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "int-run-end")) {
        /* one run of 7L ending at 5 in a block of 3 */
        static const uint8_t runs[16] = {1, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 5, 0, 0, 0};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_INTEGER, 3, 0, 1);
        add(&s, 14, 3, runs, sizeof runs);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "logical-run-end")) {
        static const uint8_t runs[16] = {1, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 1, 0, 0, 0};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_LOGICAL, 3, 0, 1);
        add(&s, RDZ_ENCODING_LOGICAL_RUN_ENDS, 3, runs, sizeof runs);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "string-record-length")) {
        /* a record claiming 100 bytes in a block holding one */
        static const uint8_t a[6] = {2, 100, 0, 0, 0, 'a'};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_CHARACTER, 1, 0, 1);
        add(&s, RDZ_ENCODING_STRING_PLAIN, 1, a, sizeof a);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "dict-index-range")) {
        /* one dictionary entry, then an element referring to entry 3 */
        static const uint8_t entry[6] = {2, 1, 0, 0, 0, 'a'};
        static const uint8_t ids[9] = {1, 0, 0, 0, 0, 0, 0, 0, 3};
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_CHARACTER, 1, 0, 2);
        add(&s, RDZ_ENCODING_STRING_DICT_ENTRIES, 1, entry, sizeof entry);
        add(&s, RDZ_ENCODING_STRING_DICT_INDICES, 1, ids, sizeof ids);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "zstd-length")) {
        /* a frame of 400 zero bytes declared as 404 */
        static const uint8_t zeros[400];
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_INTEGER, 101, 0, 1);
        add_zstd(&s, 10, 101, zeros, sizeof zeros, 404);
        s.objects = o;
        s.nobjects = 1;
    } else if (is(name, "alp-exception-position")) {
        /* a decimal vector of 100 fives whose one exception lies just past it */
        uint8_t rec[26];
        memset(rec, 0, sizeof rec);
        zb_wr_u16le(rec + 4, 1);
        zb_wr_u64le(rec + 8, 5);
        zb_wr_u16le(rec + 16, 100);
        zb_wr_u64le(rec + 18, 0x7FF00000000007A2ull);
        o[0] = object(0, RDZ_ROOT_PARENT_ID, RDZ_ROLE_ROOT, RDZ_TYPE_DOUBLE, 100, 0, 1);
        add(&s, 23, 100, rec, sizeof rec);
        s.objects = o;
        s.nobjects = 1;
    } else {
        fprintf(stderr, "probe: no case %s\n", name);
        return 2;
    }
    if (is(name, "declared-block-size")) s.block_size = 16;

    failed = write_spec(path, &s, &e);
    free(deep);
    if (failed) {
        fprintf(stderr, "probe: %s: cannot write: %s\n", name, e.message);
        return 2;
    }
    in = fopen(path, "rb");
    if (!in || fseek(in, 0, SEEK_END) || (len = ftell(in)) < 0 || fseek(in, 0, SEEK_SET)) {
        fprintf(stderr, "probe: %s: cannot read back\n", name);
        return 2;
    }
    f.n = (size_t)len;
    f.data = (uint8_t *)malloc(f.n);
    if (!f.data || fread(f.data, 1, f.n, in) != f.n) return 2;
    fclose(in);
    f.directory = (size_t)zb_rd_u64le(f.data + f.n - RDZ_TRAILER_LEN + 8);

    if (is(name, "header-checksum")) {
        zb_wr_u32le(f.data + 16, 1u << 19); /* not resealed */
    } else if (is(name, "directory-checksum")) {
        zb_wr_u64le(object_entry(&f, 0) + 16, 2); /* logical length; not resealed */
    } else if (is(name, "block-checksum")) {
        f.data[RDZ_HEADER_LEN + RDZ_BLOCK_HEADER_LEN] ^= 1; /* not resealed */
    } else if (is(name, "object-count")) {
        zb_wr_u32le(f.data + f.directory + 16, RDZ_MAX_OBJECTS + 1);
        reseal_directory_header(&f);
        reseal_directory(&f);
    } else if (is(name, "directory-length")) {
        zb_wr_u32le(f.data + f.directory + 24, 2); /* two blocks indexed, one present */
        reseal_directory_header(&f);
        reseal_directory(&f);
    } else if (is(name, "block-offsets")) {
        uint8_t *b = block_entry(&f, 1, 0, 0);
        zb_wr_u64le(b + 8, zb_rd_u64le(b + 8) + 4);
        zb_wr_u64le(b + 16, zb_rd_u64le(b + 16) + 4);
        reseal_directory(&f);
    } else if (is(name, "declared-block-size")) {
        zb_wr_u32le(f.data + 16, 8); /* the 12-byte block exceeds it */
        reseal_header(&f);
    }

    failed = read_all(f.data, f.n, &e);
    printf("%s\n", failed ? e.message : "OK");
    free(f.data);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 2) {
        for (i = 0; cases[i]; i++) printf("%s\n", cases[i]);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: probe DIR [CASE]\n");
        return 2;
    }
    return run(argv[1], argv[2]);
}
