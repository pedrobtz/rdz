#include <string.h>

#include <zubin/rw.h>

#include "rdz_logical.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define RDZ_HAVE_AVX2_KERNEL 1
#include <immintrin.h>
#endif
#if defined(__x86_64__) && defined(__SSE2__)
#define RDZ_HAVE_SSE2_KERNEL 1 /* every x86-64 CPU has SSE2 */
#include <emmintrin.h>
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#define RDZ_HAVE_NEON_KERNEL 1
#include <arm_neon.h>
#endif

enum { FALSE_STATE = 0, TRUE_STATE = 1, NA_STATE = 2 };

#define DENSE_HEADER RDZ_LOGICAL_DENSE_HEADER_LEN
#define CONSTANT_HEADER RDZ_LOGICAL_CONSTANT_HEADER_LEN
#define SPARSE_HEADER RDZ_LOGICAL_SPARSE_HEADER_LEN
#define RUN_HEADER RDZ_LOGICAL_RUN_HEADER_LEN
#define RUN_RECORD RDZ_LOGICAL_RUN_RECORD_LEN
#define PERIODIC_HEADER RDZ_LOGICAL_PERIODIC_HEADER_LEN
#define MAX_PERIOD RDZ_LOGICAL_MAX_PERIOD

/* Set only by tests, before any write (rdz_logical.h). */
static int rdz_scalar_only;

void rdz_logical_force_scalar(int on)
{
    rdz_scalar_only = on;
}

typedef struct {
    uint8_t *true_plane, *na_plane;
    size_t plane_len;
    size_t counts[3];
    size_t runs;
} rdz_planes;

static size_t packed_len(size_t n) { return (n + 3) / 4; }
static size_t plane_len(size_t n) { return (n + 7) / 8; }

static int bad_value(rdz_error *e)
{
    return rdz_invalid(e, "logical vector contains an invalid internal value");
}

static uint8_t valid_bits(size_t byte_index, size_t bitmap_len, size_t n)
{
    if (byte_index + 1 == bitmap_len && n % 8 != 0) return (uint8_t)((1u << (n % 8)) - 1u);
    return 0xffu;
}

static int rdz_ctz64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    int bit = 0;
    while (!((x >> bit) & 1u)) bit++;
    return bit;
#endif
}

static int popcount64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    int c = 0;
    while (x) {
        x &= x - 1;
        c++;
    }
    return c;
#endif
}

/* Writes the low `width` bits of a group of 32 values at byte `offset`. */
static void write_mask(uint8_t *plane, size_t offset, uint32_t bits, size_t width)
{
    size_t k, nbytes = (width + 7) / 8;
    if (width == 32) {
        zb_wr_u32le(plane + offset, bits);
        return;
    }
    for (k = 0; k < nbytes; k++) plane[offset + k] = (uint8_t)(bits >> (8 * k));
}

/* Counts and runs of a block, from its finished planes, 64 values a word:
   a run starts where a value's state differs from the one before it. Padding
   bits past n are zero in both planes, and are masked out. */
static void plane_stats(const uint8_t *tp, const uint8_t *ap, size_t n, rdz_planes *p)
{
    size_t words = n / 64, w, t = 0, a = 0, runs = n ? 1 : 0, rest = n % 64;
    uint64_t prev_t = 0, prev_a = 0;
    for (w = 0; w <= words; w++) {
        uint64_t tw, aw, change, valid;
        if (w == words) {
            size_t k;
            if (!rest) break;
            tw = aw = 0;
            for (k = 0; k < (rest + 7) / 8; k++) {
                tw |= (uint64_t)tp[w * 8 + k] << (8 * k);
                aw |= (uint64_t)ap[w * 8 + k] << (8 * k);
            }
            valid = ((uint64_t)1 << rest) - 1;
        } else {
            tw = zb_rd_u64le(tp + w * 8);
            aw = zb_rd_u64le(ap + w * 8);
            valid = ~(uint64_t)0;
        }
        t += (size_t)popcount64(tw);
        a += (size_t)popcount64(aw);
        change = ((tw ^ (tw << 1 | prev_t)) | (aw ^ (aw << 1 | prev_a))) & valid;
        if (w == 0) change &= ~(uint64_t)1; /* the first value starts the first run */
        runs += (size_t)popcount64(change);
        prev_t = tw >> 63;
        prev_a = aw >> 63;
    }
    p->counts[TRUE_STATE] = t;
    p->counts[NA_STATE] = a;
    p->counts[FALSE_STATE] = n - t - a;
    p->runs = runs;
}

/* The scalar reference for values[from, n), groups of 32 from byte offset
   from / 8 (from is a multiple of 32). */
static int classify_scalar_from(const int32_t *values, size_t from, size_t n,
                                uint8_t *tp, uint8_t *ap, rdz_error *e)
{
    size_t start;
    for (start = from; start < n; start += 32) {
        size_t width = n - start < 32 ? n - start : 32, lane;
        uint32_t t = 0, a = 0;
        for (lane = 0; lane < width; lane++) {
            int32_t v = values[start + lane];
            if (v == 1) t |= 1u << lane;
            else if (v == RDZ_LOGICAL_NA) a |= 1u << lane;
            else if (v != 0) return bad_value(e);
        }
        write_mask(tp, start / 8, t, width);
        write_mask(ap, start / 8, a, width);
    }
    return 0;
}

#ifdef RDZ_HAVE_AVX2_KERNEL
/* The valid values are 0, 1 and INT_MIN: NA is the sign bit, which movemask
   reads directly, and TRUE is bit 0, moved to the sign by a shift. A value
   is invalid when it has any other bit, or both. Groups of 64. */
__attribute__((target("avx2"))) static int classify_avx2(const int32_t *values, size_t n,
                                                         uint8_t *tp, uint8_t *ap,
                                                         size_t *done, rdz_error *e)
{
    const __m256i other = _mm256_set1_epi32(0x7ffffffe);
    size_t groups = n / 64, g;
    for (g = 0; g < groups; g++) {
        uint64_t t = 0, a = 0;
        __m256i bad = _mm256_setzero_si256();
        int lane;
        for (lane = 0; lane < 8; lane++) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(values + g * 64 +
                                                                            (size_t)lane * 8));
            __m256i up = _mm256_slli_epi32(v, 31);
            bad = _mm256_or_si256(bad, _mm256_or_si256(_mm256_and_si256(v, other),
                                                       _mm256_and_si256(v, up)));
            t |= (uint64_t)(uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(up)) << (lane * 8);
            a |= (uint64_t)(uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(v)) << (lane * 8);
        }
        if (!_mm256_testz_si256(bad, bad)) return bad_value(e);
        zb_wr_u64le(tp + g * 8, t);
        zb_wr_u64le(ap + g * 8, a);
    }
    *done = groups * 64;
    return 0;
}

static int cpu_has_avx2(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
}
#endif

#ifdef RDZ_HAVE_SSE2_KERNEL
/* As classify_avx2(), four lanes at a time: the baseline of every x86-64. */
static int classify_sse2(const int32_t *values, size_t n, uint8_t *tp, uint8_t *ap,
                         size_t *done, rdz_error *e)
{
    const __m128i other = _mm_set1_epi32(0x7ffffffe), zero = _mm_setzero_si128();
    size_t groups = n / 64, g;
    for (g = 0; g < groups; g++) {
        uint64_t t = 0, a = 0;
        __m128i bad = zero;
        int lane;
        for (lane = 0; lane < 16; lane++) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(values + g * 64 +
                                                                         (size_t)lane * 4));
            __m128i up = _mm_slli_epi32(v, 31);
            bad = _mm_or_si128(bad, _mm_or_si128(_mm_and_si128(v, other), _mm_and_si128(v, up)));
            t |= (uint64_t)(uint32_t)_mm_movemask_ps(_mm_castsi128_ps(up)) << (lane * 4);
            a |= (uint64_t)(uint32_t)_mm_movemask_ps(_mm_castsi128_ps(v)) << (lane * 4);
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(bad, zero)) != 0xffff) return bad_value(e);
        zb_wr_u64le(tp + g * 8, t);
        zb_wr_u64le(ap + g * 8, a);
    }
    *done = groups * 64;
    return 0;
}
#endif

#ifdef RDZ_HAVE_NEON_KERNEL
/* 16 values' bit 0 (TRUE) and bit 31 (NA) as two vectors of 0 or 1 bytes;
   or-ing into `bad` any value with another bit set, or both. */
static void neon_bytes16(const int32_t *values, uint8x16_t *t8, uint8x16_t *a8, uint32x4_t *bad)
{
    const uint32x4_t other = vdupq_n_u32(0x7ffffffeu);
    const uint32_t *v = (const uint32_t *)(const void *)values;
    uint32x4_t v0 = vld1q_u32(v), v1 = vld1q_u32(v + 4), v2 = vld1q_u32(v + 8),
               v3 = vld1q_u32(v + 12);
    uint32x4_t b = vorrq_u32(vandq_u32(v0, other), vandq_u32(v0, vshlq_n_u32(v0, 31)));
    b = vorrq_u32(b, vorrq_u32(vandq_u32(v1, other), vandq_u32(v1, vshlq_n_u32(v1, 31))));
    b = vorrq_u32(b, vorrq_u32(vandq_u32(v2, other), vandq_u32(v2, vshlq_n_u32(v2, 31))));
    b = vorrq_u32(b, vorrq_u32(vandq_u32(v3, other), vandq_u32(v3, vshlq_n_u32(v3, 31))));
    *bad = vorrq_u32(*bad, b);
    *t8 = vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(v0), vmovn_u32(v1))),
                      vmovn_u16(vcombine_u16(vmovn_u32(v2), vmovn_u32(v3))));
    *a8 = vshrq_n_u8(
        vcombine_u8(vshrn_n_u16(vcombine_u16(vshrn_n_u32(v0, 16), vshrn_n_u32(v1, 16)), 8),
                    vshrn_n_u16(vcombine_u16(vshrn_n_u32(v2, 16), vshrn_n_u32(v3, 16)), 8)),
        7);
}

/* Four vectors of 0 or 1 bytes (64 values) to their 64-bit mask, value 0
   lowest: each byte shifted to its bit, then pairwise sums to one word. */
static uint64_t neon_bits64(uint8x16_t b0, uint8x16_t b1, uint8x16_t b2, uint8x16_t b3)
{
    static const int8_t shifts_init[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
    const int8x16_t shifts = vld1q_s8(shifts_init);
    uint8x16_t p = vpaddq_u8(vshlq_u8(b0, shifts), vshlq_u8(b1, shifts));
    uint8x16_t q = vpaddq_u8(vshlq_u8(b2, shifts), vshlq_u8(b3, shifts));
    p = vpaddq_u8(p, q);
    p = vpaddq_u8(p, p);
    return vgetq_lane_u64(vreinterpretq_u64_u8(p), 0);
}

/* The valid values are 0, 1 and INT_MIN: TRUE is bit 0 and NA bit 31, so no
   comparison is needed; groups of 64. */
static int classify_neon(const int32_t *values, size_t n, uint8_t *tp, uint8_t *ap,
                         size_t *done, rdz_error *e)
{
    size_t groups = n / 64, g;
    for (g = 0; g < groups; g++) {
        uint8x16_t t[4], a[4];
        uint32x4_t bad = vdupq_n_u32(0);
        int k;
        for (k = 0; k < 4; k++) neon_bytes16(values + g * 64 + (size_t)k * 16, &t[k], &a[k], &bad);
        if (vmaxvq_u32(bad) != 0) return bad_value(e);
        zb_wr_u64le(tp + g * 8, neon_bits64(t[0], t[1], t[2], t[3]));
        zb_wr_u64le(ap + g * 8, neon_bits64(a[0], a[1], a[2], a[3]));
    }
    *done = groups * 64;
    return 0;
}
#endif

const char *rdz_logical_kernel(void)
{
    if (rdz_scalar_only) return "scalar";
#ifdef RDZ_HAVE_AVX2_KERNEL
    if (cpu_has_avx2()) return "avx2";
#endif
#if defined(RDZ_HAVE_SSE2_KERNEL)
    return "sse2";
#elif defined(RDZ_HAVE_NEON_KERNEL)
    return "neon";
#else
    return "scalar";
#endif
}

static int classify(const int32_t *values, size_t n, rdz_planes *p, zb_buf *scratch,
                    rdz_error *e)
{
    size_t done = 0;
    p->plane_len = plane_len(n);
    zb_buf_reset(scratch);
    if (zb_put_zeros(scratch, 2 * p->plane_len)) return rdz_memory(e, "logical bitplanes");
    p->true_plane = scratch->data;
    p->na_plane = p->plane_len ? scratch->data + p->plane_len : scratch->data; /* NULL + 0 is UB */
    if (!rdz_scalar_only) {
#ifdef RDZ_HAVE_AVX2_KERNEL
        if (cpu_has_avx2()) {
            if (classify_avx2(values, n, p->true_plane, p->na_plane, &done, e)) return 1;
        } else
#endif
        {
#ifdef RDZ_HAVE_SSE2_KERNEL
            if (classify_sse2(values, n, p->true_plane, p->na_plane, &done, e)) return 1;
#endif
        }
#ifdef RDZ_HAVE_NEON_KERNEL
        if (classify_neon(values, n, p->true_plane, p->na_plane, &done, e)) return 1;
#endif
    }
    if (classify_scalar_from(values, done, n, p->true_plane, p->na_plane, e)) return 1;
    plane_stats(p->true_plane, p->na_plane, n, p);
    return 0;
}

static uint8_t state_at(const rdz_planes *p, size_t i)
{
    uint8_t bit = (uint8_t)(1u << (i % 8));
    if (p->true_plane[i / 8] & bit) return TRUE_STATE;
    if (p->na_plane[i / 8] & bit) return NA_STATE;
    return FALSE_STATE;
}

/* ---- encoders -------------------------------------------------------------------- */

static uint8_t *put(zb_buf *out, size_t n, rdz_error *e)
{
    uint8_t *p;
    zb_buf_reset(out);
    if (zb_put_zeros(out, n)) {
        rdz_memory(e, "a logical block");
        return NULL;
    }
    p = out->data;
    return p;
}

static void encode_2bit(const int32_t *values, size_t n, uint8_t *dst)
{
    size_t i;
    memset(dst, 0, packed_len(n));
    for (i = 0; i < n; i++) {
        uint8_t s = values[i] == 1 ? TRUE_STATE : values[i] == RDZ_LOGICAL_NA ? NA_STATE : 0;
        dst[i / 4] |= (uint8_t)(s << ((i % 4) * 2));
    }
}

static int encode_constant(uint8_t state, zb_buf *out, rdz_error *e)
{
    uint8_t *p = put(out, CONSTANT_HEADER, e);
    if (!p) return 1;
    p[0] = state;
    return 0;
}

static void state_plane(const rdz_planes *p, uint8_t state, size_t n, uint8_t *dst)
{
    size_t k;
    if (state == TRUE_STATE) memcpy(dst, p->true_plane, p->plane_len);
    else if (state == NA_STATE) memcpy(dst, p->na_plane, p->plane_len);
    else {
        for (k = 0; k < p->plane_len; k++) {
            dst[k] = (uint8_t)(~(p->true_plane[k] | p->na_plane[k]) & valid_bits(k, p->plane_len, n));
        }
    }
}

static int encode_dense(const rdz_planes *p, size_t n, uint8_t def, const uint8_t *states,
                        int nstates, zb_buf *out, rdz_error *e)
{
    uint8_t *dst = put(out, DENSE_HEADER + (size_t)nstates * p->plane_len, e);
    int k;
    if (!dst) return 1;
    dst[0] = def;
    for (k = 0; k < nstates; k++) dst[1] |= (uint8_t)(1u << states[k]);
    for (k = 0; k < nstates; k++) {
        state_plane(p, states[k], n, dst + DENSE_HEADER + (size_t)k * p->plane_len);
    }
    return 0;
}

static int encode_sparse(const rdz_planes *p, size_t n, uint8_t def, const uint8_t *states,
                         int nstates, zb_buf *out, rdz_error *e)
{
    size_t c1 = p->counts[states[0]], c2 = nstates > 1 ? p->counts[states[1]] : 0;
    size_t at = SPARSE_HEADER, k, b;
    uint8_t *dst;
    if (nstates < 1 || nstates > 2 || n > 65536) {
        return rdz_invalid(e, "invalid logical sparse candidate");
    }
    dst = put(out, SPARSE_HEADER + 2 * (c1 + c2), e);
    if (!dst) return 1;
    dst[0] = def;
    dst[1] = states[0];
    dst[2] = nstates > 1 ? states[1] : 0xffu;
    zb_wr_u32le(dst + 4, (uint32_t)c1);
    zb_wr_u32le(dst + 8, (uint32_t)c2);
    for (k = 0; k < (size_t)nstates; k++) {
        /* eight plane bytes at a time: sparse planes are mostly zero */
        for (b = 0; b < p->plane_len; b += 8) {
            size_t take = p->plane_len - b < 8 ? p->plane_len - b : 8, j;
            uint64_t bits = 0;
            if (take == 8 && states[k] != FALSE_STATE) {
                bits = zb_rd_u64le((states[k] == TRUE_STATE ? p->true_plane : p->na_plane) + b);
            } else if (take == 8) {
                bits = ~(zb_rd_u64le(p->true_plane + b) | zb_rd_u64le(p->na_plane + b));
                if (b + 8 == p->plane_len && n % 8) {
                    bits &= (~(uint64_t)0 >> 8) | ((uint64_t)((1u << (n % 8)) - 1u) << 56);
                }
            } else for (j = 0; j < take; j++) {
                uint8_t byte = states[k] == TRUE_STATE ? p->true_plane[b + j]
                               : states[k] == NA_STATE ? p->na_plane[b + j]
                               : (uint8_t)(~(p->true_plane[b + j] | p->na_plane[b + j]) &
                                           valid_bits(b + j, p->plane_len, n));
                bits |= (uint64_t)byte << (8 * j);
            }
            while (bits) {
                zb_wr_u16le(dst + at, (uint16_t)(b * 8 + (size_t)rdz_ctz64(bits)));
                at += 2;
                bits &= bits - 1;
            }
        }
    }
    if (at != out->len) return rdz_invalid(e, "logical sparse count mismatch");
    return 0;
}

static int encode_runs(const rdz_planes *p, size_t n, zb_buf *out, rdz_error *e)
{
    uint8_t *dst = put(out, RUN_HEADER + p->runs * RUN_RECORD, e);
    size_t at = RUN_HEADER, i;
    uint8_t prev;
    if (!dst) return 1;
    zb_wr_u32le(dst, (uint32_t)p->runs);
    prev = state_at(p, 0);
    for (i = 1; i < n; i++) {
        uint8_t s = state_at(p, i);
        if (s != prev) {
            zb_wr_u32le(dst + at, (uint32_t)i);
            dst[at + 4] = prev;
            at += RUN_RECORD;
            prev = s;
        }
    }
    zb_wr_u32le(dst + at, (uint32_t)n);
    dst[at + 4] = prev;
    at += RUN_RECORD;
    if (at != out->len) return rdz_invalid(e, "logical run count mismatch");
    return 0;
}

/* Bits [at, at + 64) of a plane of `len` bytes, little-endian bit order;
   bits past the plane read as zero. */
static uint64_t plane_bits64(const uint8_t *plane, size_t len, size_t at)
{
    size_t byte = at / 8, k;
    unsigned shift = (unsigned)(at % 8);
    uint64_t lo = 0, hi = 0;
    if (byte + 16 <= len) {
        lo = zb_rd_u64le(plane + byte);
        hi = zb_rd_u64le(plane + byte + 8);
    } else {
        for (k = 0; k < 8 && byte + k < len; k++) lo |= (uint64_t)plane[byte + k] << (8 * k);
        for (k = 0; k < 8 && byte + 8 + k < len; k++) hi |= (uint64_t)plane[byte + 8 + k] << (8 * k);
    }
    return shift ? lo >> shift | hi << (64 - shift) : lo;
}

/* Whether a plane's first n bits repeat with this period: bit i equals bit
   i - period for every i from period on, compared 64 bits at a time. */
static int plane_periodic(const uint8_t *plane, size_t len, size_t n, size_t period)
{
    size_t i;
    for (i = period; i < n; i += 64) {
        uint64_t mask = n - i >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << (n - i)) - 1;
        if ((plane_bits64(plane, len, i) ^ plane_bits64(plane, len, i - period)) & mask) return 0;
    }
    return 1;
}

/* The shortest period (2 to MAX_PERIOD, at most a quarter of the block) of
   the values' states, from the planes: a sixteenth of the values' bytes. */
static size_t find_short_period(const rdz_planes *p, size_t n)
{
    size_t period, limit = n / 4 < MAX_PERIOD ? n / 4 : MAX_PERIOD;
    for (period = 2; period <= limit; period++) {
        if (plane_periodic(p->true_plane, p->plane_len, n, period) &&
            plane_periodic(p->na_plane, p->plane_len, n, period)) {
            return period;
        }
    }
    return 0;
}

static int encode_periodic(const int32_t *values, size_t period, zb_buf *out, rdz_error *e)
{
    uint8_t *dst = put(out, PERIODIC_HEADER + packed_len(period), e);
    if (!dst) return 1;
    zb_wr_u16le(dst, (uint16_t)period);
    encode_2bit(values, period, dst + PERIODIC_HEADER);
    return 0;
}

int rdz_logical_encode(const int32_t *values, size_t n, zb_buf *out, uint16_t *encoding,
                       zb_buf *scratch, rdz_error *e)
{
    rdz_planes p;
    uint8_t def = FALSE_STATE, states[2];
    int nstates = 0, s;
    size_t dense_len, sparse_len, run_len, period = 0, exceptions;
    if (n > RDZ_LOGICAL_BLOCK_VALUES) return rdz_limit(e, "logical block");
    if (classify(values, n, &p, scratch, e)) return 1;
    if (n == 0) {
        *encoding = RDZ_ENCODING_LOGICAL_DENSE_PLANES;
        return encode_dense(&p, 0, FALSE_STATE, states, 0, out, e);
    }
    for (s = TRUE_STATE; s <= NA_STATE; s++) {
        if (p.counts[s] > p.counts[def]) def = (uint8_t)s;
    }
    if (p.counts[def] == n) {
        *encoding = RDZ_ENCODING_LOGICAL_CONSTANT;
        return encode_constant(def, out, e);
    }
    for (s = FALSE_STATE; s <= NA_STATE; s++) {
        if (s != def && p.counts[s]) states[nstates++] = (uint8_t)s;
    }
    dense_len = DENSE_HEADER + (size_t)nstates * p.plane_len;
    exceptions = n - p.counts[def];
    sparse_len = SPARSE_HEADER + exceptions * 2;
    run_len = RUN_HEADER + p.runs * RUN_RECORD;
    if (p.runs > n / 2) period = find_short_period(&p, n);
    if (period) {
        size_t periodic_len = PERIODIC_HEADER + packed_len(period);
        if (periodic_len < dense_len && periodic_len < sparse_len && periodic_len < run_len) {
            *encoding = RDZ_ENCODING_LOGICAL_PERIODIC;
            return encode_periodic(values, period, out, e);
        }
    }
    if (run_len < dense_len && run_len <= sparse_len) {
        *encoding = RDZ_ENCODING_LOGICAL_RUN_ENDS;
        return encode_runs(&p, n, out, e);
    }
    if (sparse_len < dense_len) {
        *encoding = RDZ_ENCODING_LOGICAL_SPARSE_PATCHES;
        return encode_sparse(&p, n, def, states, nstates, out, e);
    }
    *encoding = RDZ_ENCODING_LOGICAL_DENSE_PLANES;
    return encode_dense(&p, n, def, states, nstates, out, e);
}

/* ---- decoders -------------------------------------------------------------------- */

static int32_t value_of(uint8_t state)
{
    return state == TRUE_STATE ? 1 : state == NA_STATE ? RDZ_LOGICAL_NA : 0;
}

static int decode_2bit(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    size_t i, full = n / 4, used = n % 4;
    if (len != packed_len(n)) return rdz_invalid(e, "logical block length mismatch");
    for (i = 0; i < full; i++) {
        uint8_t byte = enc[i];
        int k;
        if (byte & (byte >> 1) & 0x55) {
            return rdz_invalid(e, "logical block contains the reserved two-bit state");
        }
        for (k = 0; k < 4; k++) out[i * 4 + (size_t)k] = value_of((byte >> (k * 2)) & 3u);
    }
    if (used) {
        uint8_t last = enc[full];
        size_t k;
        if (last >> (used * 2)) return rdz_invalid(e, "logical block has nonzero unused high bits");
        if (last & (last >> 1) & 0x55) {
            return rdz_invalid(e, "logical block contains the reserved two-bit state");
        }
        for (k = 0; k < used; k++) out[full * 4 + k] = value_of((last >> (k * 2)) & 3u);
    }
    return 0;
}

static int decode_constant(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    size_t i;
    int32_t v;
    if (n == 0 || len != CONSTANT_HEADER || enc[0] > NA_STATE || enc[1] || enc[2] || enc[3]) {
        return rdz_invalid(e, "invalid logical constant block");
    }
    v = value_of(enc[0]);
    for (i = 0; i < n; i++) out[i] = v;
    return 0;
}

/* Eight values from a byte of the TRUE plane and one of the NA plane:
   1 where TRUE, INT_MIN where NA, 0 elsewhere. */
static void dense_expand8(uint8_t t, uint8_t a, int32_t *out)
{
#ifdef RDZ_HAVE_NEON_KERNEL
    static const uint32_t lo_init[4] = {1, 2, 4, 8}, hi_init[4] = {16, 32, 64, 128};
    const uint32x4_t lo = vld1q_u32(lo_init), hi = vld1q_u32(hi_init);
    const uint32x4_t one = vdupq_n_u32(1), sign = vdupq_n_u32(0x80000000u);
    const uint32x4_t vt = vdupq_n_u32(t), va = vdupq_n_u32(a);
    uint32_t *o = (uint32_t *)(void *)out;
    vst1q_u32(o, vorrq_u32(vandq_u32(vtstq_u32(vt, lo), one), vandq_u32(vtstq_u32(va, lo), sign)));
    vst1q_u32(o + 4, vorrq_u32(vandq_u32(vtstq_u32(vt, hi), one), vandq_u32(vtstq_u32(va, hi), sign)));
#elif defined(RDZ_HAVE_SSE2_KERNEL)
    const __m128i lo = _mm_set_epi32(8, 4, 2, 1), hi = _mm_set_epi32(128, 64, 32, 16);
    const __m128i one = _mm_set1_epi32(1), sign = _mm_set1_epi32((int)0x80000000u);
    const __m128i vt = _mm_set1_epi32(t), va = _mm_set1_epi32(a);
    __m128i *o = (__m128i *)(void *)out;
    _mm_storeu_si128(o, _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(vt, lo), lo), one),
                                     _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(va, lo), lo), sign)));
    _mm_storeu_si128(o + 1, _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(vt, hi), hi), one),
                                         _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(va, hi), hi), sign)));
#else
    uint32_t *o = (uint32_t *)(void *)out;
    int k;
    for (k = 0; k < 8; k++) o[k] = ((uint32_t)(t >> k) & 1u) | ((uint32_t)(a >> k) & 1u) << 31;
#endif
}

static int decode_dense(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    const uint8_t *planes[3] = {NULL, NULL, NULL};
    uint8_t def, mask;
    size_t bitmap, nplanes = 0, at = DENSE_HEADER, b, i = 0;
    int s;
    if (len < DENSE_HEADER || enc[2] || enc[3]) return rdz_invalid(e, "invalid logical dense header");
    def = enc[0];
    mask = enc[1];
    if (def > NA_STATE || (mask & ~0x07u) || (mask & (1u << def))) {
        return rdz_invalid(e, "invalid logical dense state mask");
    }
    if (n == 0) {
        if (mask || len != DENSE_HEADER || def != FALSE_STATE) {
            return rdz_invalid(e, "invalid empty logical block");
        }
        return 0;
    }
    if (!mask) return rdz_invalid(e, "non-canonical logical dense block");
    bitmap = plane_len(n);
    for (s = 0; s < 3; s++) nplanes += (mask >> s) & 1u;
    if (len != DENSE_HEADER + nplanes * bitmap) {
        return rdz_invalid(e, "logical dense block length mismatch");
    }
    for (s = FALSE_STATE; s <= NA_STATE; s++) {
        if (mask & (1u << s)) {
            planes[s] = enc + at;
            at += bitmap;
        }
    }
    if (n % 8) {
        uint8_t invalid = (uint8_t)~((1u << (n % 8)) - 1u);
        for (s = 0; s < 3; s++) {
            if (planes[s] && (planes[s][bitmap - 1] & invalid)) {
                return rdz_invalid(e, "logical dense plane has nonzero padding bits");
            }
        }
    }
    /* TRUE is 1 and NA is INT_MIN, bit 0 and bit 31, and the planes never
       overlap: each value is its TRUE bit or-ed with its NA bit moved up.
       Whole 64-bit words of the planes first, then the tail byte by byte. */
    for (b = 0; b + 8 <= bitmap && n - i >= 64; b += 8) {
        uint64_t f = planes[0] ? zb_rd_u64le(planes[0] + b) : 0,
                 t = planes[1] ? zb_rd_u64le(planes[1] + b) : 0,
                 a = planes[2] ? zb_rd_u64le(planes[2] + b) : 0, d;
        int k;
        if ((f & t) | (f & a) | (t & a)) return rdz_invalid(e, "overlapping logical dense planes");
        d = ~(f | t | a);
        if (def == TRUE_STATE) t |= d;
        if (def == NA_STATE) a |= d;
        for (k = 0; k < 8; k++) dense_expand8((uint8_t)(t >> (8 * k)), (uint8_t)(a >> (8 * k)), out + i + 8 * (size_t)k);
        i += 64;
    }
    for (; b < bitmap; b++) {
        uint8_t f = planes[0] ? planes[0][b] : 0, t = planes[1] ? planes[1][b] : 0,
                a = planes[2] ? planes[2][b] : 0, d;
        int k;
        if ((f & t) || (f & a) || (t & a)) return rdz_invalid(e, "overlapping logical dense planes");
        d = (uint8_t)(valid_bits(b, bitmap, n) & ~(f | t | a));
        if (def == TRUE_STATE) t |= d;
        if (def == NA_STATE) a |= d;
        if (n - i >= 8) {
            dense_expand8(t, a, out + i);
            i += 8;
        } else {
            for (k = 0; i < n; k++, i++) {
                out[i] = (int32_t)(((uint32_t)(t >> k) & 1u) | ((uint32_t)(a >> k) & 1u) << 31);
            }
        }
    }
    return 0;
}

static int decode_sparse(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    uint8_t def, s1, s2;
    size_t c1, c2, at = SPARSE_HEADER, i, pass;
    int32_t dv;
    if (len < SPARSE_HEADER || n == 0 || n > 65536 || enc[3] || zb_rd_u32le(enc + 12)) {
        return rdz_invalid(e, "invalid logical sparse header");
    }
    def = enc[0];
    s1 = enc[1];
    s2 = enc[2];
    if (def > NA_STATE || s1 > NA_STATE || s1 == def ||
        (s2 != 0xffu && (s2 > NA_STATE || s2 == def || s2 <= s1))) {
        return rdz_invalid(e, "invalid logical sparse states");
    }
    c1 = zb_rd_u32le(enc + 4);
    c2 = zb_rd_u32le(enc + 8);
    if (c1 == 0 || (s2 == 0xffu) != (c2 == 0)) return rdz_invalid(e, "invalid logical sparse counts");
    if (c1 > 65536 || c2 > 65536 || len != SPARSE_HEADER + 2 * (c1 + c2) || c1 + c2 >= n) {
        return rdz_invalid(e, "logical sparse block length mismatch");
    }
    dv = value_of(def);
    for (i = 0; i < n; i++) out[i] = dv;
    for (pass = 0; pass < 2; pass++) {
        size_t count = pass ? c2 : c1, k;
        int32_t v = value_of(pass ? s2 : s1);
        long prev = -1;
        for (k = 0; k < count; k++, at += 2) {
            size_t pos = zb_rd_u16le(enc + at);
            if (pos >= n || (long)pos <= prev) return rdz_invalid(e, "invalid logical sparse position");
            if (out[pos] != dv) return rdz_invalid(e, "overlapping logical sparse positions");
            out[pos] = v;
            prev = (long)pos;
        }
    }
    return 0;
}

static int decode_runs(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    size_t runs, r, start = 0, i;
    int prev = -1;
    if (len < RUN_HEADER || n == 0 || zb_rd_u32le(enc + 4)) {
        return rdz_invalid(e, "invalid logical run header");
    }
    runs = zb_rd_u32le(enc);
    if (runs == 0 || runs > n || len != RUN_HEADER + runs * RUN_RECORD) {
        return rdz_invalid(e, "logical run block length mismatch");
    }
    for (r = 0; r < runs; r++) {
        const uint8_t *rec = enc + RUN_HEADER + r * RUN_RECORD;
        size_t end = zb_rd_u32le(rec);
        uint8_t s = rec[4];
        int32_t v;
        if (end > n) return rdz_invalid(e, "a logical run ends past its block"); /* GUARD: logical-run-end */
        if (rec[5] || rec[6] || rec[7] || s > NA_STATE || prev == s || end <= start) {
            return rdz_invalid(e, "invalid logical run record");
        }
        v = value_of(s);
        for (i = start; i < end; i++) out[i] = v;
        start = end;
        prev = s;
    }
    if (start != n) return rdz_invalid(e, "logical run ends before the block");
    return 0;
}

static int decode_periodic(const uint8_t *enc, size_t len, size_t n, int32_t *out, rdz_error *e)
{
    size_t period, filled;
    if (len < PERIODIC_HEADER || zb_rd_u16le(enc + 2) || zb_rd_u32le(enc + 4)) {
        return rdz_invalid(e, "invalid logical periodic header");
    }
    period = zb_rd_u16le(enc);
    if (period < 2 || period > MAX_PERIOD || n < period * 4 ||
        len != PERIODIC_HEADER + packed_len(period)) {
        return rdz_invalid(e, "invalid logical periodic length");
    }
    if (decode_2bit(enc + PERIODIC_HEADER, len - PERIODIC_HEADER, period, out, e)) return 1;
    for (filled = period; filled < n;) {
        size_t copy = filled < n - filled ? filled : n - filled;
        memcpy(out + filled, out, copy * sizeof *out);
        filled += copy;
    }
    return 0;
}

int rdz_logical_decode(const uint8_t *encoded, size_t len, uint16_t encoding, size_t count,
                       int32_t *out, rdz_error *e)
{
    switch (encoding) {
    case RDZ_ENCODING_LOGICAL_2BIT: return decode_2bit(encoded, len, count, out, e);
    case RDZ_ENCODING_LOGICAL_CONSTANT: return decode_constant(encoded, len, count, out, e);
    case RDZ_ENCODING_LOGICAL_DENSE_PLANES: return decode_dense(encoded, len, count, out, e);
    case RDZ_ENCODING_LOGICAL_SPARSE_PATCHES: return decode_sparse(encoded, len, count, out, e);
    case RDZ_ENCODING_LOGICAL_RUN_ENDS: return decode_runs(encoded, len, count, out, e);
    case RDZ_ENCODING_LOGICAL_PERIODIC: return decode_periodic(encoded, len, count, out, e);
    default: return rdz_invalid(e, "unsupported logical block encoding");
    }
}
