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

/* Four 32-bit lanes for the integer codec's hot loops, which gcc at -O2
   does not vectorize: masks are all-ones lanes; SSE2 has no 32-bit
   min/max or unsigned compare, so they are built from compares. */
#if defined(RDZ_VEC_SHUFFLE) && defined(__aarch64__)
typedef uint32x4_t rdz_v4;
static inline rdz_v4 v4_load(const int32_t *p) { return vreinterpretq_u32_s32(vld1q_s32(p)); }
static inline void v4_store(uint32_t *p, rdz_v4 v) { vst1q_u32(p, v); }
static inline rdz_v4 v4_dup(uint32_t x) { return vdupq_n_u32(x); }
static inline rdz_v4 v4_eq(rdz_v4 a, rdz_v4 b) { return vceqq_u32(a, b); }
static inline rdz_v4 v4_and(rdz_v4 a, rdz_v4 b) { return vandq_u32(a, b); }
static inline rdz_v4 v4_or(rdz_v4 a, rdz_v4 b) { return vorrq_u32(a, b); }
static inline rdz_v4 v4_add(rdz_v4 a, rdz_v4 b) { return vaddq_u32(a, b); }
static inline rdz_v4 v4_sub(rdz_v4 a, rdz_v4 b) { return vsubq_u32(a, b); }
static inline rdz_v4 v4_gtu(rdz_v4 a, rdz_v4 b) { return vcgtq_u32(a, b); }
static inline rdz_v4 v4_sel(rdz_v4 m, rdz_v4 a, rdz_v4 b) { return vbslq_u32(m, a, b); }
static inline rdz_v4 v4_mins(rdz_v4 a, rdz_v4 b)
{
    return vreinterpretq_u32_s32(vminq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)));
}
static inline rdz_v4 v4_maxs(rdz_v4 a, rdz_v4 b)
{
    return vreinterpretq_u32_s32(vmaxq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)));
}
static inline uint32_t v4_sum(rdz_v4 a) { return vaddvq_u32(a); }
static inline int32_t v4_hmins(rdz_v4 a) { return vminvq_s32(vreinterpretq_s32_u32(a)); }
static inline int32_t v4_hmaxs(rdz_v4 a) { return vmaxvq_s32(vreinterpretq_s32_u32(a)); }
static inline uint32_t v4_any(rdz_v4 a) { return vmaxvq_u32(a); }
#define RDZ_VEC_INT 1
#elif defined(RDZ_VEC_SHUFFLE)
typedef __m128i rdz_v4;
static inline rdz_v4 v4_load(const int32_t *p) { return _mm_loadu_si128((const __m128i *)(const void *)p); }
static inline void v4_store(uint32_t *p, rdz_v4 v) { _mm_storeu_si128((__m128i *)(void *)p, v); }
static inline rdz_v4 v4_dup(uint32_t x) { return _mm_set1_epi32((int)x); }
static inline rdz_v4 v4_eq(rdz_v4 a, rdz_v4 b) { return _mm_cmpeq_epi32(a, b); }
static inline rdz_v4 v4_and(rdz_v4 a, rdz_v4 b) { return _mm_and_si128(a, b); }
static inline rdz_v4 v4_or(rdz_v4 a, rdz_v4 b) { return _mm_or_si128(a, b); }
static inline rdz_v4 v4_add(rdz_v4 a, rdz_v4 b) { return _mm_add_epi32(a, b); }
static inline rdz_v4 v4_sub(rdz_v4 a, rdz_v4 b) { return _mm_sub_epi32(a, b); }
static inline rdz_v4 v4_gtu(rdz_v4 a, rdz_v4 b)
{
    const __m128i flip = _mm_set1_epi32((int)0x80000000u);
    return _mm_cmpgt_epi32(_mm_xor_si128(a, flip), _mm_xor_si128(b, flip));
}
static inline rdz_v4 v4_sel(rdz_v4 m, rdz_v4 a, rdz_v4 b)
{
    return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
}
static inline rdz_v4 v4_mins(rdz_v4 a, rdz_v4 b) { return v4_sel(_mm_cmpgt_epi32(a, b), b, a); }
static inline rdz_v4 v4_maxs(rdz_v4 a, rdz_v4 b) { return v4_sel(_mm_cmpgt_epi32(a, b), a, b); }
static inline uint32_t v4_lane(rdz_v4 a, int k)
{
    uint32_t t[4];
    _mm_storeu_si128((__m128i *)(void *)t, a);
    return t[k];
}
static inline uint32_t v4_sum(rdz_v4 a)
{
    return v4_lane(a, 0) + v4_lane(a, 1) + v4_lane(a, 2) + v4_lane(a, 3);
}
static inline int32_t v4_hmins(rdz_v4 a)
{
    int32_t m = (int32_t)v4_lane(a, 0), k;
    for (k = 1; k < 4; k++) m = (int32_t)v4_lane(a, k) < m ? (int32_t)v4_lane(a, k) : m;
    return m;
}
static inline int32_t v4_hmaxs(rdz_v4 a)
{
    int32_t m = (int32_t)v4_lane(a, 0), k;
    for (k = 1; k < 4; k++) m = (int32_t)v4_lane(a, k) > m ? (int32_t)v4_lane(a, k) : m;
    return m;
}
static inline uint32_t v4_any(rdz_v4 a) { return (uint32_t)(_mm_movemask_epi8(a) != 0); }
#define RDZ_VEC_INT 1
#endif

/* Codes at the byte-aligned widths, 8 and 16 bits, where the LSB-first bit
   stream is plain little-endian bytes or halfwords: narrowed from four
   lanes (each code fits its width) and widened back. */
#if defined(RDZ_VEC_INT) && defined(__aarch64__)
static inline void v4_narrow8(rdz_v4 a, rdz_v4 b, rdz_v4 c, rdz_v4 d, uint8_t *dst)
{
    uint16x8_t ab = vcombine_u16(vmovn_u32(a), vmovn_u32(b)),
               cd = vcombine_u16(vmovn_u32(c), vmovn_u32(d));
    vst1q_u8(dst, vcombine_u8(vmovn_u16(ab), vmovn_u16(cd)));
}
static inline void v4_narrow16(rdz_v4 a, rdz_v4 b, uint8_t *dst)
{
    vst1q_u8(dst, vreinterpretq_u8_u16(vcombine_u16(vmovn_u32(a), vmovn_u32(b))));
}
static inline void v4_widen8x(const uint8_t *src, rdz_v4 out[4])
{
    uint8x16_t x = vld1q_u8(src);
    uint16x8_t lo = vmovl_u8(vget_low_u8(x)), hi = vmovl_u8(vget_high_u8(x));
    out[0] = vmovl_u16(vget_low_u16(lo));
    out[1] = vmovl_u16(vget_high_u16(lo));
    out[2] = vmovl_u16(vget_low_u16(hi));
    out[3] = vmovl_u16(vget_high_u16(hi));
}
static inline void v4_widen16x(const uint8_t *src, rdz_v4 out[2])
{
    uint16x8_t x = vreinterpretq_u16_u8(vld1q_u8(src));
    out[0] = vmovl_u16(vget_low_u16(x));
    out[1] = vmovl_u16(vget_high_u16(x));
}
#elif defined(RDZ_VEC_INT)
static inline void v4_narrow8(rdz_v4 a, rdz_v4 b, rdz_v4 c, rdz_v4 d, uint8_t *dst)
{
    /* codes of at most 255: the signed saturations are exact */
    _mm_storeu_si128((__m128i *)(void *)dst,
                     _mm_packus_epi16(_mm_packs_epi32(a, b), _mm_packs_epi32(c, d)));
}
static inline void v4_narrow16(rdz_v4 a, rdz_v4 b, uint8_t *dst)
{
    /* the low halves sign-extended, so that the signed pack keeps them */
    a = _mm_srai_epi32(_mm_slli_epi32(a, 16), 16);
    b = _mm_srai_epi32(_mm_slli_epi32(b, 16), 16);
    _mm_storeu_si128((__m128i *)(void *)dst, _mm_packs_epi32(a, b));
}
static inline void v4_widen8x(const uint8_t *src, rdz_v4 out[4])
{
    const __m128i z = _mm_setzero_si128();
    __m128i x = _mm_loadu_si128((const __m128i *)(const void *)src),
            lo = _mm_unpacklo_epi8(x, z), hi = _mm_unpackhi_epi8(x, z);
    out[0] = _mm_unpacklo_epi16(lo, z);
    out[1] = _mm_unpackhi_epi16(lo, z);
    out[2] = _mm_unpacklo_epi16(hi, z);
    out[3] = _mm_unpackhi_epi16(hi, z);
}
static inline void v4_widen16x(const uint8_t *src, rdz_v4 out[2])
{
    const __m128i z = _mm_setzero_si128();
    __m128i x = _mm_loadu_si128((const __m128i *)(const void *)src);
    out[0] = _mm_unpacklo_epi16(x, z);
    out[1] = _mm_unpackhi_epi16(x, z);
}
#endif

/* AVX2 (eight lanes, chosen at run time) for the integer codec's hottest
   loops on x86-64, where SSE2 lacks 32-bit min and max. Each returns how far
   it got; the four-lane and scalar loops finish the rest. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define RDZ_HAVE_AVX2_INT 1
#include <immintrin.h>

static int rdz_int_avx2(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
}

/* from i >= 1: the minimum of values other than NA, the maximum, the NA
   count and the number of values unlike the one before */
__attribute__((target("avx2"))) static size_t int_stats_avx2(const int32_t *v, size_t i, size_t n,
                                                             int32_t *mn, int32_t *mx,
                                                             uint32_t *nas, uint32_t *changes)
{
    const __m256i na8 = _mm256_set1_epi32(INT32_MIN), max8 = _mm256_set1_epi32(INT32_MAX);
    __m256i mn0 = _mm256_set1_epi32(*mn), mx0 = _mm256_set1_epi32(*mx), mn1 = mn0, mx1 = mx0;
    __m256i vna = _mm256_setzero_si256(), veq = _mm256_setzero_si256();
    size_t start = i, k;
    int32_t a[8], b[8];
    uint32_t c[8], d[8];
    for (; i + 16 <= n; i += 16) {
        __m256i x0 = _mm256_loadu_si256((const __m256i *)(const void *)(v + i)),
                p0 = _mm256_loadu_si256((const __m256i *)(const void *)(v + i - 1)),
                x1 = _mm256_loadu_si256((const __m256i *)(const void *)(v + i + 8)),
                p1 = _mm256_loadu_si256((const __m256i *)(const void *)(v + i + 7));
        __m256i n0 = _mm256_cmpeq_epi32(x0, na8), n1 = _mm256_cmpeq_epi32(x1, na8);
        mn0 = _mm256_min_epi32(mn0, _mm256_blendv_epi8(x0, max8, n0));
        mn1 = _mm256_min_epi32(mn1, _mm256_blendv_epi8(x1, max8, n1));
        mx0 = _mm256_max_epi32(mx0, x0);
        mx1 = _mm256_max_epi32(mx1, x1);
        vna = _mm256_sub_epi32(_mm256_sub_epi32(vna, n0), n1); /* a mask is -1 */
        veq = _mm256_sub_epi32(_mm256_sub_epi32(veq, _mm256_cmpeq_epi32(x0, p0)),
                               _mm256_cmpeq_epi32(x1, p1));
    }
    if (i == start) return i;
    _mm256_storeu_si256((__m256i *)(void *)a, _mm256_min_epi32(mn0, mn1));
    _mm256_storeu_si256((__m256i *)(void *)b, _mm256_max_epi32(mx0, mx1));
    _mm256_storeu_si256((__m256i *)(void *)c, vna);
    _mm256_storeu_si256((__m256i *)(void *)d, veq);
    for (k = 0; k < 8; k++) {
        *mn = a[k] < *mn ? a[k] : *mn;
        *mx = b[k] > *mx ? b[k] : *mx;
        *nas += c[k];
        *changes -= d[k];
    }
    *changes += (uint32_t)(i - start);
    return i;
}

/* frame-of-reference codes: the NA code for NA, else value - base */
__attribute__((target("avx2"))) static size_t for_codes_avx2(const int32_t *v, size_t n,
                                                             int32_t base, uint32_t na_code,
                                                             uint32_t *codes)
{
    const __m256i na8 = _mm256_set1_epi32(INT32_MIN), code8 = _mm256_set1_epi32((int)na_code),
                  base8 = _mm256_set1_epi32(base);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(const void *)(v + i));
        _mm256_storeu_si256((__m256i *)(void *)(codes + i),
                            _mm256_blendv_epi8(_mm256_sub_epi32(x, base8), code8,
                                               _mm256_cmpeq_epi32(x, na8)));
    }
    return i;
}

/* for_add_base() eight lanes at a time; or-s into *bad */
__attribute__((target("avx2"))) static size_t for_add_base_avx2(uint32_t *c, size_t n, int has_na,
                                                                uint32_t na_code, uint32_t ubase,
                                                                uint32_t lo, uint32_t span,
                                                                uint32_t *bad)
{
    const __m256i flip = _mm256_set1_epi32(INT32_MIN), na8 = _mm256_set1_epi32(INT32_MIN),
                  code8 = _mm256_set1_epi32((int)(has_na ? na_code : 0)),
                  use_na = _mm256_set1_epi32(has_na ? -1 : 0),
                  base8 = _mm256_set1_epi32((int)ubase), lo8 = _mm256_set1_epi32((int)lo),
                  span8 = _mm256_xor_si256(_mm256_set1_epi32((int)span), flip);
    __m256i vbad = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i ci = _mm256_loadu_si256((const __m256i *)(const void *)(c + i));
        __m256i na = _mm256_and_si256(use_na, _mm256_cmpeq_epi32(ci, code8));
        /* unsigned a > b as signed after flipping the sign bits */
        __m256i b = _mm256_cmpgt_epi32(_mm256_xor_si256(_mm256_sub_epi32(ci, lo8), flip), span8);
        vbad = _mm256_or_si256(vbad, _mm256_andnot_si256(na, b));
        _mm256_storeu_si256((__m256i *)(void *)(c + i),
                            _mm256_blendv_epi8(_mm256_add_epi32(ci, base8), na8, na));
    }
    *bad |= !_mm256_testz_si256(vbad, vbad);
    return i;
}
#endif

#define INT_NA INT32_MIN

/* On a little-endian host the values' bytes in memory are their file
   bytes: raw records and shuffle sources are plain copies. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define RDZ_LE_HOST 1
#endif

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

/* Inlined into each width's function, so that W is a constant there (gcc
   at -O2 would otherwise call one shared copy). */
#if defined(__GNUC__) || defined(__clang__)
#define RDZ_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define RDZ_ALWAYS_INLINE inline
#endif

static RDZ_ALWAYS_INLINE void pack32(const uint32_t *codes, unsigned W, uint8_t *dst)
{
    uint32_t out[32];
    memset(out, 0, sizeof out);
    /* code k's bits go to word k*W/32 at shift k*W%32, and spill into the
       next word when they cross it; all constants once W is */
#define RDZ_PACK_ONE(k)                                                                          \
    out[(k) * W / 32] |= codes[k] << ((k) * W % 32);                                            \
    if ((k) * W % 32 + W > 32) out[(k) * W / 32 + 1] |= codes[k] >> ((32 - (k) * W % 32) & 31);
    RDZ_REP32(RDZ_PACK_ONE)
#undef RDZ_PACK_ONE
    /* unrolled too, so that out[] stays in registers (gcc keeps a loop) */
#define RDZ_STORE_ONE(w) if ((w) < W) zb_wr_u32le(dst + 4 * (w), out[w]);
    RDZ_REP32(RDZ_STORE_ONE)
#undef RDZ_STORE_ONE
}

static RDZ_ALWAYS_INLINE void unpack32(const uint8_t *src, unsigned W, uint32_t *codes)
{
    const uint32_t mask = W == 32 ? 0xffffffffu : (1u << W) - 1u;
    uint32_t in[33];
    in[32] = 0;
#define RDZ_LOAD_ONE(w) in[w] = (w) < W ? zb_rd_u32le(src + 4 * (w)) : 0;
    RDZ_REP32(RDZ_LOAD_ONE)
#undef RDZ_LOAD_ONE
#define RDZ_UNPACK_ONE(k)                                                                        \
    codes[k] = ((in[(k) * W / 32] >> ((k) * W % 32)) |                                         \
                ((k) * W % 32 + W > 32 ? in[(k) * W / 32 + 1] << ((32 - (k) * W % 32) & 31) : 0)) & \
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
#ifdef RDZ_VEC_INT
    if (width == 8 || width == 16) {
        size_t i;
        for (i = 0; i < 32 * groups; i += 16) {
            rdz_v4 a = v4_load((const int32_t *)(const void *)(c + i)),
                   b = v4_load((const int32_t *)(const void *)(c + i + 4)),
                   x = v4_load((const int32_t *)(const void *)(c + i + 8)),
                   y = v4_load((const int32_t *)(const void *)(c + i + 12));
            if (width == 8) {
                v4_narrow8(a, b, x, y, dst + i);
            } else {
                v4_narrow16(a, b, dst + 2 * i);
                v4_narrow16(x, y, dst + 2 * i + 16);
            }
        }
        return groups * 32;
    }
#endif
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
#ifdef RDZ_VEC_INT
    if (width == 8 || width == 16) {
        size_t i;
        for (i = 0; i < 32 * groups; i += 16) {
            rdz_v4 x[4];
            if (width == 8) {
                v4_widen8x(src + i, x);
            } else {
                v4_widen16x(src + 2 * i, x);
                v4_widen16x(src + 2 * i + 16, x + 2);
            }
            v4_store(c + i, x[0]);
            v4_store(c + i + 4, x[1]);
            v4_store(c + i + 8, x[2]);
            v4_store(c + i + 12, x[3]);
        }
        return groups * 32;
    }
#endif
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
    if (zb_buf_reserve(out, at - len + 4 * (n ? n : 1))) { /* room past len */
        rdz_memory(e, "a numeric block");
        return NULL;
    }
    return (uint32_t *)(void *)(out->data + at);
}

int rdz_int_encode(const int32_t *v, size_t n, int compressing, zb_buf *out, uint16_t *encoding,
                   rdz_error *e)
{
    int32_t lo, hi;
    int has;
    return rdz_int_encode_range(v, n, compressing, out, encoding, &lo, &hi, &has, e);
}

int rdz_int_encode_range(const int32_t *v, size_t n, int compressing, zb_buf *out,
                         uint16_t *encoding, int32_t *lo_out, int32_t *hi_out, int *has_values,
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
        i = 1;
#ifdef RDZ_HAVE_AVX2_INT
        if (n >= 17 && rdz_int_avx2()) {
            i = int_stats_avx2(v, i, n, &mn, &mx, &na_count, &changes);
        }
#endif
#ifdef RDZ_VEC_INT
        if (n - i >= 4) {
            const rdz_v4 na4 = v4_dup((uint32_t)INT_NA), max4 = v4_dup((uint32_t)INT32_MAX);
            rdz_v4 vmn = v4_dup((uint32_t)mn), vmx = v4_dup((uint32_t)mx), vna = v4_dup(0),
                   vch = v4_dup(0), all = v4_dup(0xffffffffu);
            for (; i + 4 <= n; i += 4) {
                rdz_v4 cur = v4_load(v + i), prev = v4_load(v + i - 1), isna = v4_eq(cur, na4);
                vmn = v4_mins(vmn, v4_sel(isna, max4, cur));
                vmx = v4_maxs(vmx, cur);
                vna = v4_sub(vna, isna); /* a mask is -1 */
                vch = v4_sub(vch, v4_sel(v4_eq(cur, prev), v4_dup(0), all));
            }
            mn = v4_hmins(vmn);
            mx = v4_hmaxs(vmx);
            na_count += v4_sum(vna);
            changes += v4_sum(vch);
        }
#endif
        for (; i < n; i++) {
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
        *has_values = nas < n;
        *lo_out = mn;
        *hi_out = mx;
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
            i = probe;
#ifdef RDZ_VEC_INT
            /* no NA here, and a range of at most 2^31 - 1 keeps every
               difference in 32 bits: four lanes */
            if ((int64_t)hi - lo <= INT32_MAX && i + 4 <= n) {
                rdz_v4 vlo = v4_dup((uint32_t)(int32_t)dlo), vhi = v4_dup((uint32_t)(int32_t)dhi);
                for (; i + 4 <= n; i += 4) {
                    rdz_v4 d = v4_sub(v4_load(v + i), v4_load(v + i - 1));
                    vlo = v4_mins(vlo, d);
                    vhi = v4_maxs(vhi, d);
                }
                dlo = v4_hmins(vlo) < dlo ? v4_hmins(vlo) : dlo;
                dhi = v4_hmaxs(vhi) > dhi ? v4_hmaxs(vhi) : dhi;
            }
#endif
            for (; i < n; i++) {
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
        i = 1;
#ifdef RDZ_VEC_INT
        {
            /* modulo 2^32 the code is the same, and fits by delta_width */
            const rdz_v4 lo4 = v4_dup((uint32_t)dlo);
            for (; i + 4 <= n; i += 4) {
                v4_store(codes + i - 1, v4_sub(v4_sub(v4_load(v + i), v4_load(v + i - 1)), lo4));
            }
        }
#endif
        for (; i < n; i++) codes[i - 1] = (uint32_t)((int64_t)v[i] - (int64_t)v[i - 1] - dlo);
        dst = out->data;
        dst[0] = (uint8_t)delta_width;
        zb_wr_u32le(dst + 4, (uint32_t)v[0]);
        zb_wr_u64le(dst + 8, (uint64_t)dlo);
        pack_codes(codes, n - 1, delta_width, dst + RDZ_INT_DELTA_HEADER);
        *encoding = RDZ_ENCODING_INT_DELTA;
        return 0;
    }
    if (n && for_len < raw_len && !compressing && for_width && for_width <= 16) {
        /* level 0 stores codes of up to 16 bits as bytes or halfwords, which
           a pass narrows to and widens from as fast as it copies them; the
           encoding was chosen by the smallest width, which only grows here */
        unsigned width = for_width <= 8 ? 8 : 16;
        uint32_t na_code = (1u << width) - 1u, base = nas == n ? 0 : (uint32_t)(int32_t)lo;
        size_t len = RDZ_INT_FOR_HEADER + packed_bytes(n, width);
        uint8_t *codes;
        if (!(dst = reserve(out, len, e))) return 1;
        memset(dst, 0, RDZ_INT_FOR_HEADER);
        dst[0] = (uint8_t)width;
        dst[1] = nas ? 1 : 0;
        zb_wr_u32le(dst + 4, base);
        codes = dst + RDZ_INT_FOR_HEADER;
        i = 0;
#ifdef RDZ_VEC_INT
        {
            const rdz_v4 na4 = v4_dup((uint32_t)INT_NA), code4 = v4_dup(na_code),
                         base4 = v4_dup(base);
#define RDZ_CODE4(k) v4_sel(v4_eq(v4_load(v + i + (k)), na4), code4, v4_sub(v4_load(v + i + (k)), base4))
            for (; i + 16 <= n; i += 16) {
                if (width == 8) {
                    v4_narrow8(RDZ_CODE4(0), RDZ_CODE4(4), RDZ_CODE4(8), RDZ_CODE4(12), codes + i);
                } else {
                    v4_narrow16(RDZ_CODE4(0), RDZ_CODE4(4), codes + 2 * i);
                    v4_narrow16(RDZ_CODE4(8), RDZ_CODE4(12), codes + 2 * i + 16);
                }
            }
#undef RDZ_CODE4
        }
#endif
        for (; i < n; i++) {
            uint32_t c = v[i] == INT_NA ? na_code : (uint32_t)v[i] - base;
            codes[width / 8 * i] = (uint8_t)c;
            if (width == 16) codes[2 * i + 1] = (uint8_t)(c >> 8);
        }
        *encoding = RDZ_ENCODING_INT_FOR;
        return 0;
    }
    if (n && for_len < raw_len) {
        uint32_t na_code = for_width == 32 ? 0xffffffffu : (uint32_t)((1ull << for_width) - 1);
        int32_t base = nas == n ? 0 : (int32_t)lo;
        uint32_t *codes;
        if (!(codes = codes_after(out, for_len, n, e))) return 1;
        i = 0;
#ifdef RDZ_HAVE_AVX2_INT
        if (rdz_int_avx2()) i = for_codes_avx2(v, n, base, na_code, codes);
#endif
#ifdef RDZ_VEC_INT
        {
            const rdz_v4 na4 = v4_dup((uint32_t)INT_NA), code4 = v4_dup(na_code),
                         base4 = v4_dup((uint32_t)base);
            for (; i + 4 <= n; i += 4) {
                rdz_v4 x = v4_load(v + i);
                v4_store(codes + i, v4_sel(v4_eq(x, na4), code4, v4_sub(x, base4)));
            }
        }
#endif
        for (; i < n; i++) {
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
#ifdef RDZ_LE_HOST
        memcpy(le, v, raw_len);
#else
        for (i = 0; i < n; i++) zb_wr_u32le(le + 4 * i, (uint32_t)v[i]);
#endif
        shuffle(le, n, 4, dst);
        *encoding = RDZ_ENCODING_INT_SHUFFLE;
    } else {
#ifdef RDZ_LE_HOST
        if (n) memcpy(dst, v, raw_len);
#else
        for (i = 0; i < n; i++) zb_wr_u32le(dst + 4 * i, (uint32_t)v[i]);
#endif
        *encoding = RDZ_ENCODING_INT_RAW;
    }
    return 0;
}

/* A frame-of-reference block's codes c[0, n) into values in place: base
   added, the NA code to NA; in 32-bit lanes, base + code overflows exactly
   when the code exceeds INT32_MAX - base, and lands on NA only as code 0 on
   a base of INT32_MIN. With top (a factor's level count, else 0), a value
   outside 1..top is out of range too, in the same pass. Returns nonzero
   when a value is out of range. */
/* Base added to the codes in place, NA codes to NA; nonzero if a code
   other than NA is outside [lo, lo + span], the codes that land in range
   (for_code_range()). */
static uint32_t for_add_base(uint32_t *c, size_t n, int has_na, uint32_t na_code, uint32_t ubase,
                             uint32_t lo, uint32_t span)
{
    uint32_t bad = 0;
    size_t i = 0;
#ifdef RDZ_HAVE_AVX2_INT
    if (rdz_int_avx2()) i = for_add_base_avx2(c, n, has_na, na_code, ubase, lo, span, &bad);
#endif
#ifdef RDZ_VEC_INT
    {
        const rdz_v4 code4 = v4_dup(has_na ? na_code : 0), use_na = v4_dup(has_na ? ~0u : 0),
                     na4 = v4_dup((uint32_t)INT_NA), base4 = v4_dup(ubase), lo4 = v4_dup(lo),
                     span4 = v4_dup(span);
        rdz_v4 vbad = v4_dup(0);
        for (; i + 4 <= n; i += 4) {
            rdz_v4 ci = v4_load((const int32_t *)(const void *)(c + i));
            rdz_v4 na = v4_and(use_na, v4_eq(ci, code4));
            vbad = v4_or(vbad, v4_sel(na, v4_dup(0), v4_gtu(v4_sub(ci, lo4), span4)));
            v4_store(c + i, v4_sel(na, na4, v4_add(ci, base4)));
        }
        bad |= v4_any(vbad) != 0;
    }
#endif
    for (; i < n; i++) {
        uint32_t ci = c[i], na = has_na && ci == na_code;
        bad |= (na ^ 1u) & (ci - lo > span);
        c[i] = na ? (uint32_t)INT_NA : ubase + ci;
    }
    return bad;
}

#ifdef RDZ_VEC_INT
/* for_add_base() straight from codes of 8 or 16 bits, which are widened in
   registers, never stored as codes: the whole groups of 16; returns how
   many it did and or-s into *bad. */
static size_t for_add_base_aligned(const uint8_t *src, unsigned width, size_t n, uint32_t *c,
                                   int has_na, uint32_t na_code, uint32_t ubase, uint32_t lo,
                                   uint32_t span, uint32_t *bad)
{
    const rdz_v4 code4 = v4_dup(has_na ? na_code : 0), use_na = v4_dup(has_na ? ~0u : 0),
                 na4 = v4_dup((uint32_t)INT_NA), base4 = v4_dup(ubase), lo4 = v4_dup(lo),
                 span4 = v4_dup(span), zero4 = v4_dup(0);
    rdz_v4 vbad = zero4;
    size_t i;
    for (i = 0; i + 16 <= n; i += 16) {
        rdz_v4 x[4];
        int k;
        if (width == 8) {
            v4_widen8x(src + i, x);
        } else {
            v4_widen16x(src + 2 * i, x);
            v4_widen16x(src + 2 * i + 16, x + 2);
        }
        for (k = 0; k < 4; k++) {
            rdz_v4 na = v4_and(use_na, v4_eq(x[k], code4));
            vbad = v4_or(vbad, v4_sel(na, zero4, v4_gtu(v4_sub(x[k], lo4), span4)));
            v4_store(c + i + 4 * k, v4_sel(na, na4, v4_add(x[k], base4)));
        }
    }
    *bad |= v4_any(vbad) != 0;
    return i;
}
#endif

/* The codes of a FOR block on `base` that decode in range, as [lo, lo +
   span]: base + code must not overflow int32 nor land on NA (INT32_MIN),
   and for a factor (top levels) must be 1..top. Zero if there are none. */
static int for_code_range(int64_t base, uint32_t top, uint32_t *lo, uint32_t *span)
{
    int64_t a = base == INT32_MIN ? 1 : 0, b = INT32_MAX - base;
    if (top) {
        if (1 - base > a) a = 1 - base;
        if ((int64_t)top - base < b) b = (int64_t)top - base;
    }
    if (b < a) return 0;
    *lo = (uint32_t)a;
    *span = (uint32_t)(b - a);
    return 1;
}

int rdz_int_codes_ok(const int32_t *v, size_t n, uint64_t nlev)
{
    uint32_t top = nlev > (uint64_t)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)nlev, bad = 0;
    size_t i = 0;
    if (!top) { /* no levels: only NA */
        for (; i < n; i++) bad |= (uint32_t)(v[i] != INT_NA);
        return !bad;
    }
#ifdef RDZ_VEC_INT
    {
        /* code - 1, unsigned, is below top exactly for 1..top; NA is exempt */
        const rdz_v4 one = v4_dup(1), na4 = v4_dup((uint32_t)INT_NA), top1 = v4_dup(top - 1),
                     zero = v4_dup(0);
        rdz_v4 vbad = zero;
        for (; i + 4 <= n; i += 4) {
            rdz_v4 x = v4_load(v + i);
            vbad = v4_or(vbad, v4_sel(v4_eq(x, na4), zero, v4_gtu(v4_sub(x, one), top1)));
        }
        bad = v4_any(vbad) != 0;
    }
#endif
    for (; i < n; i++) bad |= (uint32_t)(v[i] != INT_NA) & (uint32_t)((uint32_t)v[i] - 1u >= top);
    return !bad;
}

static int bad_int(rdz_error *e) { return rdz_invalid(e, "invalid integer block"); }

/* rdz_int_decode(), and with top (a factor's level count, else 0) the
   frame-of-reference record checks its values against 1..top as it adds the
   base, and sets *checked. */
/* Codes decoded and used a chunk at a time: whole groups of 32, and small
   enough to stay in a core's level-2 cache (64 KiB of codes). */
#define RDZ_DECODE_CHUNK ((size_t)16384)

/* A delta block of width 0 is an arithmetic progression from out[0] =
   value in steps of dmin: in range throughout exactly when its last value
   is (it is monotone), and then exact in 32-bit lanes. Nonzero if not. */
static int delta_progression(int32_t *out, size_t n, int64_t value, int64_t dmin)
{
    uint64_t span = (uint64_t)(dmin < 0 ? -dmin : dmin);
    uint32_t step = (uint32_t)dmin, x = (uint32_t)value;
    int64_t last;
    size_t i = 1;
    /* past 2^32 - 1 in all the progression leaves int32 (and the product
       below could overflow) */
    if (span && (uint64_t)(n - 1) > UINT32_MAX / span) return 1;
    last = value + (int64_t)(n - 1) * dmin;
    if (last <= INT32_MIN || last > INT32_MAX) return 1;
#ifdef RDZ_VEC_INT
    if (n >= 9) {
        uint32_t *o = (uint32_t *)(void *)out;
        const int32_t first[4] = {(int32_t)(x + step), (int32_t)(x + 2u * step),
                                  (int32_t)(x + 3u * step), (int32_t)(x + 4u * step)};
        const rdz_v4 step4 = v4_dup(4u * step), step8 = v4_dup(8u * step);
        rdz_v4 a = v4_load(first), b = v4_add(a, step4);
        for (; i + 8 <= n; i += 8) {
            v4_store(o + i, a);
            v4_store(o + i + 4, b);
            a = v4_add(a, step8);
            b = v4_add(b, step8);
        }
        x += (uint32_t)(i - 1) * step;
    }
#endif
    for (; i < n; i++) {
        x += step;
        out[i] = (int32_t)x;
    }
    return 0;
}

static int int_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                      uint32_t top, int *checked, rdz_error *e);

int rdz_int_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                   rdz_error *e)
{
    int checked = 0;
    return int_decode(enc, len, encoding, n, out, 0, &checked, e);
}

int rdz_factor_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                      uint64_t nlev, rdz_error *e)
{
    uint32_t top = nlev > (uint64_t)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)nlev;
    int checked = 0;
    if (int_decode(enc, len, encoding, n, out, top, &checked, e)) return 1;
    if (!checked && !rdz_int_codes_ok(out, n, nlev)) {
        return rdz_invalid(e, "factor code outside its levels");
    }
    return 0;
}

static int int_decode(const uint8_t *enc, size_t len, uint16_t encoding, size_t n, int32_t *out,
                      uint32_t top, int *checked, rdz_error *e)
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
#ifdef RDZ_LE_HOST
        if (n) memcpy(out, enc, 4 * n);
#else
        for (i = 0; i < n; i++) out[i] = (int32_t)zb_rd_u32le(enc + 4 * i);
#endif
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
            uint32_t ubase = (uint32_t)base, lo = 0, span = 0, bad = 0;
            /* no code decodes in range: then only NA codes may appear */
            int none = !for_code_range(base, top, &lo, &span);
            size_t at = 0;
#ifdef RDZ_VEC_INT
            /* codes of 8 or 16 bits: one pass, the rest as below */
            if (!none && (width == 8 || width == 16)) {
                at = for_add_base_aligned(enc + RDZ_INT_FOR_HEADER, width, n, c, has_na, na_code,
                                          ubase, lo, span, &bad);
            }
#endif
            /* chunk by chunk, so that the codes are still in cache when the
               base is added; a chunk is whole groups and starts on a byte */
            for (; at < n; at += RDZ_DECODE_CHUNK) {
                size_t m = n - at < RDZ_DECODE_CHUNK ? n - at : RDZ_DECODE_CHUNK,
                       skip = at / 8 * width;
                unpack_codes(enc + RDZ_INT_FOR_HEADER + skip, len - RDZ_INT_FOR_HEADER - skip, m,
                             width, c + at);
                bad |= for_add_base(c + at, m, has_na, na_code, ubase, lo, span);
            }
            if (none) {
                for (at = 0; at < n && !bad; at++) bad = out[at] != INT_NA;
            }
            *checked = top != 0;
            if (bad) {
                return top ? rdz_invalid(e, "factor code outside its levels") : bad_int(e);
            }
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
        if (width == 0) return delta_progression(out, n, value, dmin) ? bad_int(e) : 0;
        {
            uint32_t *c = (uint32_t *)(void *)(out + 1);
            size_t at;
            int bad = 0;
            /* as FOR: the codes of a chunk, then their running sum in place */
            for (at = 0; at < n - 1; at += RDZ_DECODE_CHUNK) {
                size_t m = n - 1 - at < RDZ_DECODE_CHUNK ? n - 1 - at : RDZ_DECODE_CHUNK,
                       skip = at / 8 * width;
                unpack_codes(enc + RDZ_INT_DELTA_HEADER + skip, len - RDZ_INT_DELTA_HEADER - skip,
                             m, width, c + at);
                for (i = at; i < at + m; i++) {
                    value += dmin + (int64_t)c[i];
                    bad |= value <= INT32_MIN || value > INT32_MAX;
                    c[i] = (uint32_t)(int32_t)value;
                }
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
            i = start;
#ifdef RDZ_VEC_INT
            {
                const rdz_v4 v4 = v4_dup((uint32_t)value);
                for (; i + 16 <= end; i += 16) {
                    uint32_t *o = (uint32_t *)(void *)(out + i);
                    v4_store(o, v4);
                    v4_store(o + 4, v4);
                    v4_store(o + 8, v4);
                    v4_store(o + 12, v4);
                }
            }
#endif
            for (; i < end; i++) out[i] = value;
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
       than runs and raw; level 0 keeps its plain copy */
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
#ifdef RDZ_LE_HOST
        memcpy(le, v, raw_len);
#else
        for (i = 0; i < n; i++) zb_wr_u64le(le + 8 * i, bits_of(v[i]));
#endif
        shuffle(le, n, 8, dst);
        *encoding = RDZ_ENCODING_DBL_SHUFFLE;
    } else {
#ifdef RDZ_LE_HOST
        if (n) memcpy(dst, v, raw_len);
#else
        for (i = 0; i < n; i++) zb_wr_u64le(dst + 8 * i, bits_of(v[i]));
#endif
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
#ifdef RDZ_LE_HOST
        if (n) memcpy(out, enc, 8 * n);
#else
        for (i = 0; i < n; i++) out[i] = double_of(zb_rd_u64le(enc + 8 * i));
#endif
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
