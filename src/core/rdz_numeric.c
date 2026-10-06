#include <string.h>

#include <zubin/rw.h>

#include "rdz_alp.h"
#include "rdz_numeric.h"

/* Byte transposes for the shuffled layouts with 16-byte vectors, where a
   value's bytes in memory are its little-endian bytes: NEON on
   little-endian arm64, SSE2 on x86-64 (every one has it). Each provides
   load, store, zip (interleave the bytes of a and b: low halves, then high)
   and uzp (de-interleave a then b: even bytes, then odd), its inverse. */
#if defined(__aarch64__) && defined(__ARM_NEON) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define RDZ_VEC_SHUFFLE 1
#include <arm_neon.h>
typedef uint8x16_t rdz_v16;
static inline rdz_v16 v16_load(const uint8_t *p) { return vld1q_u8(p); }
static inline void v16_store(uint8_t *p, rdz_v16 v) { vst1q_u8(p, v); }
static inline void v16_zip(rdz_v16 a, rdz_v16 b, rdz_v16 *lo, rdz_v16 *hi)
{
    uint8x16x2_t z = vzipq_u8(a, b);
    *lo = z.val[0];
    *hi = z.val[1];
}
static inline void v16_uzp(rdz_v16 a, rdz_v16 b, rdz_v16 *even, rdz_v16 *odd)
{
    uint8x16x2_t u = vuzpq_u8(a, b);
    *even = u.val[0];
    *odd = u.val[1];
}
#elif defined(__x86_64__) && defined(__SSE2__)
#define RDZ_VEC_SHUFFLE 1
#include <emmintrin.h>
typedef __m128i rdz_v16;
static inline rdz_v16 v16_load(const uint8_t *p) { return _mm_loadu_si128((const __m128i *)(const void *)p); }
static inline void v16_store(uint8_t *p, rdz_v16 v) { _mm_storeu_si128((__m128i *)(void *)p, v); }
static inline void v16_zip(rdz_v16 a, rdz_v16 b, rdz_v16 *lo, rdz_v16 *hi)
{
    *lo = _mm_unpacklo_epi8(a, b);
    *hi = _mm_unpackhi_epi8(a, b);
}
static inline void v16_uzp(rdz_v16 a, rdz_v16 b, rdz_v16 *even, rdz_v16 *odd)
{
    const __m128i low = _mm_set1_epi16(0x00ff);
    *even = _mm_packus_epi16(_mm_and_si128(a, low), _mm_and_si128(b, low));
    *odd = _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
}
#endif

#define INT_NA INT32_MIN

static size_t packed_bytes(size_t n, unsigned width)
{
    /* n <= 262,144 and width <= 32, so this cannot wrap */
    return (n * width + 7) / 8;
}

static unsigned bit_length(uint64_t x)
{
    unsigned w = 0;
    while (x) {
        w++;
        x >>= 1;
    }
    return w;
}

static uint8_t *reserve(zb_buf *out, size_t n, rdz_error *e)
{
    zb_buf_reset(out);
    if (zb_put_zeros(out, n)) {
        rdz_memory(e, "a numeric block");
        return NULL;
    }
    /* an empty record may have no storage: any non-NULL pointer will do */
    return out->data ? out->data : (uint8_t *)out;
}

/* 32 codes of W bits occupy exactly W 32-bit words. With W a constant (one
   function per width, below) every index and shift is a constant, unrolled
   by RDZ_REP32: the scalar form of FastLanes-style bit packing. */
#define RDZ_REP32(X)                                                                             \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) \
    X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static inline void pack32(const uint32_t *codes, unsigned W, uint8_t *dst)
{
    uint32_t out[32];
    unsigned w;
    memset(out, 0, sizeof out);
    /* code k's bits go to word k*W/32 at shift k*W%32, and spill into the
       next word when they cross it; all constants once W is */
#define RDZ_PACK_ONE(k)                                                                          \
    out[(k) * W / 32] |= codes[k] << ((k) * W % 32);                                            \
    if ((k) * W % 32 + W > 32) out[(k) * W / 32 + 1] |= codes[k] >> (32 - (k) * W % 32);
    RDZ_REP32(RDZ_PACK_ONE)
#undef RDZ_PACK_ONE
    for (w = 0; w < W; w++) zb_wr_u32le(dst + 4 * w, out[w]);
}

static inline void unpack32(const uint8_t *src, unsigned W, uint32_t *codes)
{
    const uint32_t mask = W == 32 ? 0xffffffffu : (1u << W) - 1u;
    uint32_t in[33];
    unsigned w;
    for (w = 0; w < W; w++) in[w] = zb_rd_u32le(src + 4 * w);
    in[W] = 0;
#define RDZ_UNPACK_ONE(k)                                                                        \
    codes[k] = ((in[(k) * W / 32] >> ((k) * W % 32)) |                                         \
                ((k) * W % 32 + W > 32 ? in[(k) * W / 32 + 1] << (32 - (k) * W % 32) : 0)) &   \
               mask;
    RDZ_REP32(RDZ_UNPACK_ONE)
#undef RDZ_UNPACK_ONE
}

#define RDZ_PACK_WIDTHS(X)                                                                       \
    X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16)      \
    X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)  \
    X(32)
#define RDZ_PACK_FN(W)                                                                           \
    static void pack_w##W(const uint32_t *c, size_t groups, uint8_t *dst)                        \
    {                                                                                            \
        size_t g;                                                                                \
        for (g = 0; g < groups; g++) pack32(c + 32 * g, W, dst + 4 * W * g);                     \
    }                                                                                            \
    static void unpack_w##W(const uint8_t *src, size_t groups, uint32_t *c)                      \
    {                                                                                            \
        size_t g;                                                                                \
        for (g = 0; g < groups; g++) unpack32(src + 4 * W * g, W, c + 32 * g);                   \
    }
RDZ_PACK_WIDTHS(RDZ_PACK_FN)
#undef RDZ_PACK_FN

/* Whole groups of 32 codes, at width 1 to 32; returns the codes done. */
static size_t pack_groups(const uint32_t *c, size_t n, unsigned width, uint8_t *dst)
{
    size_t groups = n / 32;
    switch (width) {
#define RDZ_PACK_CASE(W) case W: pack_w##W(c, groups, dst); break;
        RDZ_PACK_WIDTHS(RDZ_PACK_CASE)
#undef RDZ_PACK_CASE
    default: return 0;
    }
    return groups * 32;
}

static size_t unpack_groups(const uint8_t *src, size_t n, unsigned width, uint32_t *c)
{
    size_t groups = n / 32;
    switch (width) {
#define RDZ_UNPACK_CASE(W) case W: unpack_w##W(src, groups, c); break;
        RDZ_PACK_WIDTHS(RDZ_UNPACK_CASE)
#undef RDZ_UNPACK_CASE
    default: return 0;
    }
    return groups * 32;
}

/* LSB-first bit packing: code i occupies bits [i * width, (i + 1) * width)
   of a little-endian bit stream. pack_codes() writes exactly
   packed_bytes(n, width) bytes: whole 32-bit words from a 64-bit
   accumulator, then the bytes of the rest (codes fit in `width` bits). */
static void pack_codes(const uint32_t *codes, size_t n, unsigned width, uint8_t *dst)
{
    uint64_t acc = 0;
    unsigned have = 0;
    size_t i;
    if (!width) return;
    /* whole groups of 32 end on a word boundary; the rest streams on */
    i = pack_groups(codes, n, width, dst);
    dst += i / 8 * width;
    for (; i < n; i++) {
        acc |= (uint64_t)codes[i] << have;
        have += width;
        if (have >= 32) {
            zb_wr_u32le(dst, (uint32_t)acc);
            dst += 4;
            acc >>= 32;
            have -= 32;
        }
    }
    for (; have > 0; have = have > 8 ? have - 8 : 0) {
        *dst++ = (uint8_t)acc;
        acc >>= 8;
    }
}

/* The n codes of a packed stream of `width` bits and `len` bytes, into
   codes[]: a 64-bit accumulator refilled 32 bits at a time (bytes at the
   end of the stream, which is never read past). */
static void unpack_codes(const uint8_t *src, size_t len, size_t n, unsigned width,
                         uint32_t *codes)
{
    const uint8_t *end = src + len;
    uint64_t acc = 0, mask = width == 32 ? 0xffffffffull : ((1ull << width) - 1);
    unsigned have = 0;
    size_t i;
    if (!width) {
        memset(codes, 0, n * sizeof *codes);
        return;
    }
    i = unpack_groups(src, n, width, codes);
    src += i / 8 * width;
    for (; i < n; i++) {
        if (have < width) {
            uint64_t word;
            if (end - src >= 4) {
                word = zb_rd_u32le(src);
                src += 4;
            } else {
                unsigned k;
                word = 0;
                for (k = 0; src < end; k++) word |= (uint64_t)*src++ << (8 * k);
            }
            acc |= word << have;
            have += 32;
        }
        codes[i] = (uint32_t)(acc & mask);
        acc >>= width;
        have -= width;
    }
}

/* Unused high bits of the last packed byte must be zero (canonical). */
static int padding_clear(const uint8_t *src, size_t n, unsigned width)
{
    size_t bits = n * width;
    if (bits % 8 == 0) return 1;
    return (src[bits / 8] >> (bits % 8)) == 0;
}

#ifdef RDZ_VEC_SHUFFLE
/* 16 values at a time: three rounds of byte de-interleaving (uzp) split 16
   eight-byte values into their 8 byte planes; zip, its inverse, joins them. */
static size_t shuffle8_vec(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t i;
    for (i = 0; i + 16 <= n; i += 16) {
        const uint8_t *s = src + 8 * i;
        rdz_v16 a0, a1, b0, b1, c0, c1, d0, d1, e00, e01, e10, e11, o00, o01, o10, o11;
        rdz_v16 p0, p4, p2, p6, p1, p5, p3, p7;
        v16_uzp(v16_load(s), v16_load(s + 16), &a0, &a1);
        v16_uzp(v16_load(s + 32), v16_load(s + 48), &b0, &b1);
        v16_uzp(v16_load(s + 64), v16_load(s + 80), &c0, &c1);
        v16_uzp(v16_load(s + 96), v16_load(s + 112), &d0, &d1);
        v16_uzp(a0, b0, &e00, &e01);
        v16_uzp(c0, d0, &e10, &e11);
        v16_uzp(a1, b1, &o00, &o01);
        v16_uzp(c1, d1, &o10, &o11);
        v16_uzp(e00, e10, &p0, &p4);
        v16_uzp(e01, e11, &p2, &p6);
        v16_uzp(o00, o10, &p1, &p5);
        v16_uzp(o01, o11, &p3, &p7);
        v16_store(dst + i, p0);
        v16_store(dst + n + i, p1);
        v16_store(dst + 2 * n + i, p2);
        v16_store(dst + 3 * n + i, p3);
        v16_store(dst + 4 * n + i, p4);
        v16_store(dst + 5 * n + i, p5);
        v16_store(dst + 6 * n + i, p6);
        v16_store(dst + 7 * n + i, p7);
    }
    return i;
}

static size_t unshuffle8_vec(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t i;
    for (i = 0; i + 16 <= n; i += 16) {
        uint8_t *d = dst + 8 * i;
        rdz_v16 m0l, m0h, m2l, m2h, m1l, m1h, m3l, m3h, e00, e01, e10, e11, o00, o01, o10, o11;
        rdz_v16 a0, a1, b0, b1, c0, c1, d0, d1;
        v16_zip(v16_load(src + i), v16_load(src + 4 * n + i), &m0l, &m0h);
        v16_zip(v16_load(src + 2 * n + i), v16_load(src + 6 * n + i), &m2l, &m2h);
        v16_zip(v16_load(src + n + i), v16_load(src + 5 * n + i), &m1l, &m1h);
        v16_zip(v16_load(src + 3 * n + i), v16_load(src + 7 * n + i), &m3l, &m3h);
        v16_zip(m0l, m2l, &e00, &e01);
        v16_zip(m0h, m2h, &e10, &e11);
        v16_zip(m1l, m3l, &o00, &o01);
        v16_zip(m1h, m3h, &o10, &o11);
        v16_zip(e00, o00, &a0, &a1);
        v16_zip(e01, o01, &b0, &b1);
        v16_zip(e10, o10, &c0, &c1);
        v16_zip(e11, o11, &d0, &d1);
        v16_store(d, a0);
        v16_store(d + 16, a1);
        v16_store(d + 32, b0);
        v16_store(d + 48, b1);
        v16_store(d + 64, c0);
        v16_store(d + 80, c1);
        v16_store(d + 96, d0);
        v16_store(d + 112, d1);
    }
    return i;
}

static size_t shuffle4_vec(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t i;
    for (i = 0; i + 16 <= n; i += 16) {
        const uint8_t *s = src + 4 * i;
        rdz_v16 a0, a1, b0, b1, p0, p2, p1, p3;
        v16_uzp(v16_load(s), v16_load(s + 16), &a0, &a1);
        v16_uzp(v16_load(s + 32), v16_load(s + 48), &b0, &b1);
        v16_uzp(a0, b0, &p0, &p2);
        v16_uzp(a1, b1, &p1, &p3);
        v16_store(dst + i, p0);
        v16_store(dst + n + i, p1);
        v16_store(dst + 2 * n + i, p2);
        v16_store(dst + 3 * n + i, p3);
    }
    return i;
}

static size_t unshuffle4_vec(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t i;
    for (i = 0; i + 16 <= n; i += 16) {
        uint8_t *d = dst + 4 * i;
        rdz_v16 m0l, m0h, m1l, m1h, a0, a1, b0, b1;
        v16_zip(v16_load(src + i), v16_load(src + 2 * n + i), &m0l, &m0h);
        v16_zip(v16_load(src + n + i), v16_load(src + 3 * n + i), &m1l, &m1h);
        v16_zip(m0l, m1l, &a0, &a1);
        v16_zip(m0h, m1h, &b0, &b1);
        v16_store(d, a0);
        v16_store(d + 16, a1);
        v16_store(d + 32, b0);
        v16_store(d + 48, b1);
    }
    return i;
}
#endif

/* Values of `width` (4 or 8) little-endian bytes into byte planes: plane k
   holds every value's byte k. */
static void shuffle(const uint8_t *src, size_t n, size_t width, uint8_t *dst)
{
    size_t i = 0, k;
#ifdef RDZ_VEC_SHUFFLE
    i = width == 8 ? shuffle8_vec(src, n, dst) : shuffle4_vec(src, n, dst);
#endif
    for (; i < n; i++) {
        for (k = 0; k < width; k++) dst[k * n + i] = src[i * width + k];
    }
}

/* The inverse: n values of `width` little-endian bytes from their planes.
   Returns how many it wrote as native values (the whole groups of 16 with
   vector transposes, else none: the caller assembles the rest). */
static size_t unshuffle_native(const uint8_t *src, size_t n, size_t width, uint8_t *dst)
{
#ifdef RDZ_VEC_SHUFFLE
    return width == 8 ? unshuffle8_vec(src, n, dst) : unshuffle4_vec(src, n, dst);
#else
    (void)src;
    (void)n;
    (void)width;
    (void)dst;
    return 0;
#endif
}

/* ---- integers --------------------------------------------------------------------- */

int rdz_int_length_ok(uint16_t encoding, uint64_t n, uint64_t len)
{
    switch (encoding) {
    case RDZ_ENCODING_INT_RAW:
    case RDZ_ENCODING_INT_SHUFFLE: return len == 4 * n;
    case RDZ_ENCODING_INT_FOR: return n != 0 && len >= RDZ_INT_FOR_HEADER && len <= RDZ_INT_FOR_HEADER + 4 * n;
    case RDZ_ENCODING_INT_DELTA:
        return n >= 2 && len >= RDZ_INT_DELTA_HEADER && len <= RDZ_INT_DELTA_HEADER + 4 * n;
    case RDZ_ENCODING_INT_RUNS:
        return n != 0 && len >= RDZ_RUNS_HEADER + RDZ_INT_RUN_RECORD &&
               len <= RDZ_RUNS_HEADER + RDZ_INT_RUN_RECORD * n;
    default: return 0;
    }
}

/* A zeroed record of `len` bytes in `out`, and scratch for n codes past
   it (4-byte aligned), which pack_codes() reads while writing the record. */
static uint32_t *codes_after(zb_buf *out, size_t len, size_t n, rdz_error *e)
{
    size_t at = (len + 3) & ~(size_t)3;
    if (!reserve(out, len, e)) return NULL;
    if (zb_buf_reserve(out, at + 4 * (n ? n : 1))) {
        rdz_memory(e, "a numeric block");
        return NULL;
    }
    return (uint32_t *)(void *)(out->data + at);
}

int rdz_int_encode(const int32_t *v, size_t n, int compressing, zb_buf *out, uint16_t *encoding,
                   rdz_error *e)
{
    int64_t lo = INT32_MAX, hi = INT32_MIN, dlo = INT64_MAX, dhi = INT64_MIN;
    size_t i, runs = n ? 1 : 0, nas = 0;
    size_t raw_len = 4 * n, for_len = SIZE_MAX, delta_len = SIZE_MAX, runs_len;
    unsigned for_width = 0, delta_width = 0;
    uint8_t *dst;

    /* one pass without branches, which compilers vectorize: NA is
       INT32_MIN, so the maximum ignores it, and the minimum skips it */
    {
        int32_t mn = INT32_MAX, mx = INT32_MIN;
        uint32_t changes = 0, na_count = 0; /* a block's n fits: 32-bit lanes */
        if (n) {
            na_count = v[0] == INT_NA;
            mn = v[0] == INT_NA ? INT32_MAX : v[0];
            mx = v[0];
        }
        for (i = 1; i < n; i++) {
            int32_t x = v[i] == INT_NA ? INT32_MAX : v[i];
            na_count += v[i] == INT_NA;
            changes += v[i] != v[i - 1];
            mn = x < mn ? x : mn;
            mx = v[i] > mx ? v[i] : mx;
        }
        runs += changes;
        nas = na_count;
        if (nas < n) {
            lo = mn;
            hi = mx;
        }
    }
    if (n) {
        uint64_t range = nas == n ? 0 : (uint64_t)(hi - lo);
        for_width = bit_length(nas ? range + 1 : range);
    }
    /* deltas only without NA, and only when they can beat FOR: a prefix's
       delta range bounds the block's from below */
    if (nas == 0 && n >= 2) {
        size_t probe = n < 1024 ? n : 1024;
        for (i = 1; i < probe; i++) {
            int64_t d = (int64_t)v[i] - (int64_t)v[i - 1];
            dlo = d < dlo ? d : dlo;
            dhi = d > dhi ? d : dhi;
        }
        if (bit_length((uint64_t)(dhi - dlo)) < for_width) {
            for (i = probe; i < n; i++) {
                int64_t d = (int64_t)v[i] - (int64_t)v[i - 1];
                dlo = d < dlo ? d : dlo;
                dhi = d > dhi ? d : dhi;
            }
        } else {
            dlo = INT64_MAX; /* no delta record */
        }
    }
    runs_len = RDZ_RUNS_HEADER + RDZ_INT_RUN_RECORD * runs;
    if (n) for_len = RDZ_INT_FOR_HEADER + packed_bytes(n, for_width);
    if (n >= 2 && nas == 0 && dlo != INT64_MAX) {
        delta_width = bit_length((uint64_t)(dhi - dlo));
        if (delta_width <= 32) delta_len = RDZ_INT_DELTA_HEADER + packed_bytes(n - 1, delta_width);
    }

    /* the smallest; ties go to the cheaper decoder: runs, delta, FOR, raw */
    if (n && runs_len <= delta_len && runs_len <= for_len && runs_len <= raw_len) {
        size_t at = RDZ_RUNS_HEADER;
        if (!(dst = reserve(out, runs_len, e))) return 1;
        zb_wr_u32le(dst, (uint32_t)runs);
        for (i = 0; i < n;) {
            int32_t value = v[i];
            size_t j = i + 1;
            while (j < n && v[j] == value) j++; /* a tight scan along the run */
            zb_wr_u32le(dst + at, (uint32_t)value);
            zb_wr_u32le(dst + at + 4, (uint32_t)j);
            at += RDZ_INT_RUN_RECORD;
            i = j;
        }
        *encoding = RDZ_ENCODING_INT_RUNS;
        return 0;
    }
    if (delta_len < for_len && delta_len < raw_len) {
        uint32_t *codes;
        if (!(codes = codes_after(out, delta_len, n, e))) return 1;
        for (i = 1; i < n; i++) codes[i - 1] = (uint32_t)((int64_t)v[i] - (int64_t)v[i - 1] - dlo);
        dst = out->data;
        dst[0] = (uint8_t)delta_width;
        zb_wr_u32le(dst + 4, (uint32_t)v[0]);
        zb_wr_u64le(dst + 8, (uint64_t)dlo);
        pack_codes(codes, n - 1, delta_width, dst + RDZ_INT_DELTA_HEADER);
        *encoding = RDZ_ENCODING_INT_DELTA;
        return 0;
    }
    if (n && for_len < raw_len) {
        uint32_t na_code = for_width == 32 ? 0xffffffffu : (uint32_t)((1ull << for_width) - 1);
        int32_t base = nas == n ? 0 : (int32_t)lo;
        uint32_t *codes;
        if (!(codes = codes_after(out, for_len, n, e))) return 1;
        for (i = 0; i < n; i++) {
            codes[i] = v[i] == INT_NA ? na_code : (uint32_t)v[i] - (uint32_t)base;
        }
        dst = out->data;
        dst[0] = (uint8_t)for_width;
        dst[1] = nas ? 1 : 0;
        zb_wr_u32le(dst + 4, (uint32_t)base);
        pack_codes(codes, n, for_width, dst + RDZ_INT_FOR_HEADER);
        *encoding = RDZ_ENCODING_INT_FOR;
        return 0;
    }
    if (!(dst = reserve(out, raw_len, e))) return 1;
    if (compressing && n) { /* an empty block is raw */
        uint8_t *le;
        if (zb_buf_reserve(out, raw_len)) return rdz_memory(e, "a numeric block");
        dst = out->data;
        le = out->data + raw_len; /* scratch past the record */
        for (i = 0; i < n; i++) zb_wr_u32le(le + 4 * i, (uint32_t)v[i]);
        shuffle(le, n, 4, dst);
        *encoding = RDZ_ENCODING_INT_SHUFFLE;
    } else {
        for (i = 0; i < n; i++) zb_wr_u32le(dst + 4 * i, (uint32_t)v[i]);
        *encoding = RDZ_ENCODING_INT_RAW;
    }
    return 0;
}

static int bad_int(rdz_error *e) { return rdz_invalid(e, "invalid integer block"); }

int rdz_int_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                   rdz_error *e)
{
    size_t i;
    /* an empty block: only the layouts that hold nothing but values */
    if (!rdz_int_length_ok(encoding, n, len) &&
        !(n == 0 && len == 0 &&
          (encoding == RDZ_ENCODING_INT_RAW || encoding == RDZ_ENCODING_INT_SHUFFLE))) {
        return rdz_invalid(e, "integer block length mismatch");
    }
    switch (encoding) {
    case RDZ_ENCODING_INT_RAW:
        for (i = 0; i < n; i++) out[i] = (int32_t)zb_rd_u32le(enc + 4 * i);
        return 0;
    case RDZ_ENCODING_INT_SHUFFLE:
        for (i = unshuffle_native(enc, n, 4, (uint8_t *)(void *)out); i < n; i++) {
            out[i] = (int32_t)((uint32_t)enc[i] | (uint32_t)enc[n + i] << 8 |
                               (uint32_t)enc[2 * n + i] << 16 | (uint32_t)enc[3 * n + i] << 24);
        }
        return 0;
    case RDZ_ENCODING_INT_FOR: {
        unsigned width = enc[0];
        int has_na = enc[1];
        int64_t base = (int32_t)zb_rd_u32le(enc + 4);
        uint32_t na_code;
        if (width > 32 || enc[1] > 1 || enc[2] || enc[3] ||
            len != RDZ_INT_FOR_HEADER + packed_bytes(n, width) ||
            !padding_clear(enc + RDZ_INT_FOR_HEADER, n, width) || (has_na && width == 0)) {
            return bad_int(e);
        }
        na_code = width == 32 ? 0xffffffffu : (uint32_t)((1ull << width) - 1);
        {
            /* the codes into out, then base added in place, branch-free
               (NA codes are common and unpredictable) */
            uint32_t *c = (uint32_t *)(void *)out;
            /* in 32-bit lanes: base + code overflows exactly when the code
               exceeds INT32_MAX - base, and lands on NA only as code 0 on a
               base of INT32_MIN */
            uint32_t limit = (uint32_t)(INT32_MAX - base), ubase = (uint32_t)base,
                     zero_bad = base == INT32_MIN, bad = 0;
            unpack_codes(enc + RDZ_INT_FOR_HEADER, len - RDZ_INT_FOR_HEADER, n, width, c);
            if (has_na) {
                for (i = 0; i < n; i++) {
                    uint32_t ci = c[i], na = ci == na_code;
                    bad |= (na ^ 1u) & ((ci > limit) | (zero_bad & (ci == 0)));
                    c[i] = na ? (uint32_t)INT_NA : ubase + ci;
                }
            } else {
                for (i = 0; i < n; i++) {
                    uint32_t ci = c[i];
                    bad |= (ci > limit) | (zero_bad & (ci == 0));
                    c[i] = ubase + ci;
                }
            }
            if (bad) return bad_int(e);
        }
        return 0;
    }
    case RDZ_ENCODING_INT_DELTA: {
        unsigned width = enc[0];
        int64_t value = (int32_t)zb_rd_u32le(enc + 4), dmin = (int64_t)zb_rd_u64le(enc + 8);
        if (width > 32) return rdz_invalid(e, "invalid delta width"); /* GUARD: delta-width */
        if (enc[1] || enc[2] || enc[3] || value == INT32_MIN ||
            dmin < -(int64_t)UINT32_MAX || dmin > (int64_t)UINT32_MAX ||
            len != RDZ_INT_DELTA_HEADER + packed_bytes(n - 1, width) ||
            !padding_clear(enc + RDZ_INT_DELTA_HEADER, n - 1, width)) {
            return bad_int(e);
        }
        out[0] = (int32_t)value;
        {
            uint32_t *c = (uint32_t *)(void *)(out + 1);
            int bad = 0;
            unpack_codes(enc + RDZ_INT_DELTA_HEADER, len - RDZ_INT_DELTA_HEADER, n - 1, width, c);
            for (i = 1; i < n; i++) {
                value += dmin + (int64_t)c[i - 1];
                bad |= value <= INT32_MIN || value > INT32_MAX;
                out[i] = (int32_t)value;
            }
            if (bad) return bad_int(e);
        }
        return 0;
    }
    case RDZ_ENCODING_INT_RUNS: {
        size_t runs = zb_rd_u32le(enc), r, start = 0;
        if (zb_rd_u32le(enc + 4) || runs == 0 || runs > n ||
            len != RDZ_RUNS_HEADER + RDZ_INT_RUN_RECORD * runs) {
            return bad_int(e);
        }
        for (r = 0; r < runs; r++) {
            const uint8_t *rec = enc + RDZ_RUNS_HEADER + r * RDZ_INT_RUN_RECORD;
            int32_t value = (int32_t)zb_rd_u32le(rec);
            size_t end = zb_rd_u32le(rec + 4);
            if (end > n) return rdz_invalid(e, "an integer run ends past its block"); /* GUARD: int-run-end */
            if (end <= start || (r && value == out[start - 1])) return bad_int(e);
            for (i = start; i < end; i++) out[i] = value;
            start = end;
        }
        return start == n ? 0 : bad_int(e);
    }
    default:
        return rdz_invalid(e, "unsupported integer block encoding");
    }
}

/* ---- doubles ---------------------------------------------------------------------- */

static uint64_t bits_of(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

static double double_of(uint64_t b)
{
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

int rdz_dbl_length_ok(uint16_t encoding, uint64_t n, uint64_t len)
{
    switch (encoding) {
    case RDZ_ENCODING_DBL_RAW:
    case RDZ_ENCODING_DBL_SHUFFLE: return len == 8 * n;
    case RDZ_ENCODING_DBL_RUNS:
        return n != 0 && len >= RDZ_RUNS_HEADER + RDZ_DBL_RUN_RECORD &&
               len <= RDZ_RUNS_HEADER + RDZ_DBL_RUN_RECORD * n;
    case RDZ_ENCODING_DBL_DECIMAL: return rdz_alp_length_ok(n, len);
    default: return 0;
    }
}

int rdz_dbl_encode(const double *v, size_t n, int compressing, zb_buf *out, uint16_t *encoding,
                   rdz_error *e)
{
    size_t i, runs = n ? 1 : 0, raw_len = 8 * n, runs_len;
    uint8_t *dst;
    for (i = 1; i < n; i++) {
        if (bits_of(v[i]) != bits_of(v[i - 1])) runs++;
    }
    runs_len = RDZ_RUNS_HEADER + RDZ_DBL_RUN_RECORD * runs;
    /* decimals (ALP) when the block will be compressed and they are smaller
       than runs and raw; the speed preset keeps its plain copy */
    if (compressing && n) {
        int used;
        if (rdz_alp_encode(v, n, runs_len < raw_len ? runs_len : raw_len, out, &used, e)) return 1;
        if (used) {
            *encoding = RDZ_ENCODING_DBL_DECIMAL;
            return 0;
        }
    }
    if (n && runs_len < raw_len) {
        size_t at = RDZ_RUNS_HEADER;
        if (!(dst = reserve(out, runs_len, e))) return 1;
        zb_wr_u32le(dst, (uint32_t)runs);
        for (i = 1; i <= n; i++) {
            if (i == n || bits_of(v[i]) != bits_of(v[i - 1])) {
                zb_wr_u64le(dst + at, bits_of(v[i - 1]));
                zb_wr_u32le(dst + at + 8, (uint32_t)i);
                at += RDZ_DBL_RUN_RECORD;
            }
        }
        *encoding = RDZ_ENCODING_DBL_RUNS;
        return 0;
    }
    if (!(dst = reserve(out, raw_len, e))) return 1;
    if (compressing && n) { /* an empty block is raw */
        uint8_t *le;
        if (zb_buf_reserve(out, raw_len)) return rdz_memory(e, "a numeric block");
        dst = out->data;
        le = out->data + raw_len;
        for (i = 0; i < n; i++) zb_wr_u64le(le + 8 * i, bits_of(v[i]));
        shuffle(le, n, 8, dst);
        *encoding = RDZ_ENCODING_DBL_SHUFFLE;
    } else {
        for (i = 0; i < n; i++) zb_wr_u64le(dst + 8 * i, bits_of(v[i]));
        *encoding = RDZ_ENCODING_DBL_RAW;
    }
    return 0;
}

int rdz_dbl_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, double *out,
                   rdz_error *e)
{
    size_t i;
    /* an empty block: only the layouts that hold nothing but values */
    if (!rdz_dbl_length_ok(encoding, n, len) &&
        !(n == 0 && len == 0 &&
          (encoding == RDZ_ENCODING_DBL_RAW || encoding == RDZ_ENCODING_DBL_SHUFFLE))) {
        return rdz_invalid(e, "double block length mismatch");
    }
    switch (encoding) {
    case RDZ_ENCODING_DBL_RAW:
        for (i = 0; i < n; i++) out[i] = double_of(zb_rd_u64le(enc + 8 * i));
        return 0;
    case RDZ_ENCODING_DBL_SHUFFLE:
        for (i = unshuffle_native(enc, n, 8, (uint8_t *)(void *)out); i < n; i++) {
            uint64_t b = 0;
            int k;
            for (k = 0; k < 8; k++) b |= (uint64_t)enc[(size_t)k * n + i] << (8 * k);
            out[i] = double_of(b);
        }
        return 0;
    case RDZ_ENCODING_DBL_RUNS: {
        size_t runs = zb_rd_u32le(enc), r, start = 0;
        uint64_t prev = 0;
        if (zb_rd_u32le(enc + 4) || runs == 0 || runs > n ||
            len != RDZ_RUNS_HEADER + RDZ_DBL_RUN_RECORD * runs) {
            return rdz_invalid(e, "invalid double block");
        }
        for (r = 0; r < runs; r++) {
            const uint8_t *rec = enc + RDZ_RUNS_HEADER + r * RDZ_DBL_RUN_RECORD;
            uint64_t b = zb_rd_u64le(rec);
            size_t end = zb_rd_u32le(rec + 8);
            double d = double_of(b);
            if (zb_rd_u32le(rec + 12) || end <= start || end > n || (r && b == prev)) {
                return rdz_invalid(e, "invalid double block");
            }
            for (i = start; i < end; i++) out[i] = d;
            start = end;
            prev = b;
        }
        return start == n ? 0 : rdz_invalid(e, "invalid double block");
    }
    case RDZ_ENCODING_DBL_DECIMAL:
        return rdz_alp_decode(enc, len, n, out, e);
    default:
        return rdz_invalid(e, "unsupported double block encoding");
    }
}
