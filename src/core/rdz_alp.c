#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zubin/rw.h>

#include "rdz_alp.h"

/* A vector: e:u8 f:u8 width:u8 flags:u8 exceptions:u16 reserved:u16 base:i64,
   then (delta) first:i64, the bit-packed codes, the exceptions' positions
   (u16, increasing) and their bits (u64). */
#define ALP_HEADER 16u
#define ALP_MAX_E  18
#define ALP_DELTA  1u

static const double P10[ALP_MAX_E + 1] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,
                                          1e7,  1e8,  1e9,  1e10, 1e11, 1e12, 1e13,
                                          1e14, 1e15, 1e16, 1e17, 1e18};
static const double N10[ALP_MAX_E + 1] = {1e0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,
                                          1e-7,  1e-8,  1e-9,  1e-10, 1e-11, 1e-12, 1e-13,
                                          1e-14, 1e-15, 1e-16, 1e-17, 1e-18};
static const int64_t I10[ALP_MAX_E + 1] = {1LL,
                                           10LL,
                                           100LL,
                                           1000LL,
                                           10000LL,
                                           100000LL,
                                           1000000LL,
                                           10000000LL,
                                           100000000LL,
                                           1000000000LL,
                                           10000000000LL,
                                           100000000000LL,
                                           1000000000000LL,
                                           10000000000000LL,
                                           100000000000000LL,
                                           1000000000000000LL,
                                           10000000000000000LL,
                                           100000000000000000LL,
                                           1000000000000000000LL};
/* the largest |n| with |n * 10^f| < 2^53, by f */
static const int64_t LIMIT[ALP_MAX_E + 1] = {9007199254740991LL,
                                             900719925474099LL,
                                             90071992547409LL,
                                             9007199254740LL,
                                             900719925474LL,
                                             90071992547LL,
                                             9007199254LL,
                                             900719925LL,
                                             90071992LL,
                                             9007199LL,
                                             900719LL,
                                             90071LL,
                                             9007LL,
                                             900LL,
                                             90LL,
                                             9LL,
                                             0LL,
                                             0LL,
                                             0LL};

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

/* two's complement without implementation-defined conversion */
static int64_t signed_of(uint64_t u)
{
    return u <= (uint64_t)INT64_MAX ? (int64_t)u : -(int64_t)(~u) - 1;
}

/* The double nearest m / 10^e, |m| < 2^53, rounding once. */
static double alp_value(int64_t m, int e)
{
#if defined(RDZ_ALP_STRTOD) || (defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0)
    /* x87 would round the quotient to 64 bits and then to 53: strtod()
       rounds once (the string has no decimal point, so no locale) */
    char s[48];
    if (e == 0) return (double)m;
    snprintf(s, sizeof s, "%llde-%d", (long long)m, e);
    return strtod(s, NULL);
#else
    return (double)m / P10[e];
#endif
}

/* x as a decimal n under (e, f), when it is exactly one */
static int alp_try(double x, int e, int f, int64_t *n)
{
    double t = x * P10[e] * N10[f];
    int64_t k;
    if (!(t > -2251799813685248.0 && t < 2251799813685248.0)) return 0; /* 2^51; not NaN */
    k = (int64_t)(t < 0 ? t - 0.5 : t + 0.5);
    if (k > LIMIT[f] || k < -LIMIT[f]) return 0;
    if (bits_of(alp_value(k * I10[f], e)) != bits_of(x)) return 0;
    *n = k;
    return 1;
}

static unsigned width_of(uint64_t range)
{
    unsigned w = 0;
    while (range) {
        w++;
        range >>= 1;
    }
    return w;
}

/* ---- encoding ---------------------------------------------------------------------- */

typedef struct {
    int e, f;
} alp_combo;

/* estimated bytes for m values under (e, f) */
static double alp_estimate(const double *x, size_t m, int e, int f)
{
    int64_t lo = INT64_MAX, hi = INT64_MIN, k;
    size_t i, exceptions = 0;
    for (i = 0; i < m; i++) {
        if (!alp_try(x[i], e, f, &k)) {
            exceptions++;
            continue;
        }
        if (k < lo) lo = k;
        if (k > hi) hi = k;
    }
    if (exceptions == m) return 1e300;
    return (double)m * width_of((uint64_t)hi - (uint64_t)lo) / 8.0 + 10.0 * (double)exceptions;
}

/* up to `want` evenly spread values of x[0, n) */
static size_t alp_sample(const double *x, size_t n, size_t want, double *s)
{
    size_t step = n > want ? n / want : 1, i, m = 0;
    for (i = 0; i < n && m < want; i += step) s[m++] = x[i];
    return m;
}

/* The block's candidate (e, f): the most frequent best ones over up to eight
   sampled vectors (ALP's first-level sampling). */
static int alp_candidates(const double *x, size_t n, alp_combo *out, int want)
{
    unsigned char count[ALP_MAX_E + 1][ALP_MAX_E + 1];
    size_t nvec = (n + RDZ_ALP_VECTOR - 1) / RDZ_ALP_VECTOR, step = nvec > 8 ? nvec / 8 : 1, v;
    int got = 0, e, f;
    memset(count, 0, sizeof count);
    for (v = 0; v < nvec; v += step) {
        double s[32], best = 1e301;
        size_t start = v * RDZ_ALP_VECTOR,
               len = n - start < RDZ_ALP_VECTOR ? n - start : RDZ_ALP_VECTOR,
               m = alp_sample(x + start, len, 32, s);
        int be = 0, bf = 0;
        for (e = ALP_MAX_E; e >= 0; e--) {
            for (f = e; f >= 0; f--) {
                double c = alp_estimate(s, m, e, f);
                if (c < best || (c == best && e - f < be - bf)) {
                    best = c;
                    be = e;
                    bf = f;
                }
            }
        }
        /* the first sampled vector decides whether ALP is worth sampling
           further: under 48 bits a value */
        if (v == 0 && best * 8.0 >= 48.0 * (double)m) return 0;
        if (best < 1e300) count[be][bf]++;
    }
    while (got < want) {
        int bc = 0, be = -1, bf = -1;
        for (e = 0; e <= ALP_MAX_E; e++) {
            for (f = 0; f <= e; f++) {
                if (count[e][f] > bc) {
                    bc = count[e][f];
                    be = e;
                    bf = f;
                }
            }
        }
        if (be < 0) break;
        out[got].e = be;
        out[got].f = bf;
        got++;
        count[be][bf] = 0;
    }
    return got;
}

/* LSB-first packing of 64-bit codes into zeroed memory */
static size_t alp_pack(const uint64_t *codes, size_t n, unsigned w, uint8_t *dst)
{
    size_t i, bit = 0;
    if (w == 0) return 0;
    for (i = 0; i < n; i++, bit += w) {
        uint64_t c = codes[i];
        size_t byte = bit / 8;
        unsigned shift = (unsigned)(bit % 8), done = 0;
        /* the low (8 - shift) bits into the first byte, then whole bytes */
        dst[byte] |= (uint8_t)(c << shift);
        done = 8 - shift;
        while (done < w) {
            dst[++byte] |= (uint8_t)(c >> done);
            done += 8;
        }
    }
    return (n * w + 7) / 8;
}

/* One vector into dst; its length. */
static size_t alp_vector_encode(const double *x, size_t n, const alp_combo *cand, int ncand,
                                uint8_t *dst)
{
    int64_t ints[RDZ_ALP_VECTOR];
    uint64_t codes[RDZ_ALP_VECTOR];
    uint16_t pos[RDZ_ALP_VECTOR];
    size_t i, exceptions = 0, at = ALP_HEADER, ncodes = n, k;
    int best = 0, c, e, f, delta = 0;
    unsigned w;
    int64_t lo = INT64_MAX, hi = INT64_MIN, base, last = 0;
    int have = 0;
    if (ncand > 1) { /* the second-level choice, on a sample of this vector */
        double s[32], bc = 1e301;
        size_t m = alp_sample(x, n, 32, s);
        for (c = 0; c < ncand; c++) {
            double est = alp_estimate(s, m, cand[c].e, cand[c].f);
            if (est < bc) {
                bc = est;
                best = c;
            }
        }
    }
    e = cand[best].e;
    f = cand[best].f;
    for (i = 0; i < n; i++) {
        if (!alp_try(x[i], e, f, &ints[i])) {
            pos[exceptions++] = (uint16_t)i;
            ints[i] = 0;
        } else if (!have) {
            last = ints[i];
            have = 1;
        }
    }
    /* an exception takes its predecessor's value (or the first decimal's),
       so it widens neither the frame nor the deltas */
    for (i = 0, k = 0; i < n; i++) {
        if (k < exceptions && pos[k] == i) {
            ints[i] = last;
            k++;
        } else {
            last = ints[i];
        }
    }
    for (i = 0; i < n; i++) {
        if (ints[i] < lo) lo = ints[i];
        if (ints[i] > hi) hi = ints[i];
    }
    w = width_of((uint64_t)hi - (uint64_t)lo);
    base = lo;
    if (n >= 2) { /* delta when the steps are narrower than the frame */
        int64_t dmin = INT64_MAX, dmax = INT64_MIN;
        for (i = 1; i < n; i++) {
            /* |ints| < 2^53, so a step fits */
            int64_t d = ints[i] - ints[i - 1];
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        if (width_of((uint64_t)dmax - (uint64_t)dmin) + 1 < w) {
            delta = 1;
            w = width_of((uint64_t)dmax - (uint64_t)dmin);
            base = dmin;
            ncodes = n - 1;
            for (i = 1; i < n; i++) codes[i - 1] = (uint64_t)(ints[i] - ints[i - 1] - dmin);
        }
    }
    if (!delta) {
        for (i = 0; i < n; i++) codes[i] = (uint64_t)ints[i] - (uint64_t)lo;
    }
    dst[0] = (uint8_t)e;
    dst[1] = (uint8_t)f;
    dst[2] = (uint8_t)w;
    dst[3] = (uint8_t)(delta ? ALP_DELTA : 0u);
    zb_wr_u16le(dst + 4, (uint16_t)exceptions);
    zb_wr_u16le(dst + 6, 0);
    zb_wr_u64le(dst + 8, (uint64_t)base);
    if (delta) {
        zb_wr_u64le(dst + at, (uint64_t)ints[0]);
        at += 8;
    }
    at += alp_pack(codes, ncodes, w, dst + at);
    for (i = 0; i < exceptions; i++, at += 2) zb_wr_u16le(dst + at, pos[i]);
    for (i = 0; i < exceptions; i++, at += 8) zb_wr_u64le(dst + at, bits_of(x[pos[i]]));
    return at;
}

int rdz_alp_encode(const double *v, size_t n, size_t limit, zb_buf *out, int *used, rdz_error *e)
{
    alp_combo cand[5];
    double s[256];
    size_t m, nvec = (n + RDZ_ALP_VECTOR - 1) / RDZ_ALP_VECTOR, cap, at = 0, i;
    int ncand;
    *used = 0;
    if (n < 64) return 0; /* too few values to beat the other records */
    ncand = alp_candidates(v, n, cand, 5);
    if (!ncand) return 0;
    /* promising: under 48 bits a value on a block-wide sample */
    m = alp_sample(v, n, 256, s);
    if (alp_estimate(s, m, cand[0].e, cand[0].f) * 8.0 >= 48.0 * (double)m) return 0;
    /* at most every vector's header and first, every code at 64 bits and
       every value an exception */
    cap = nvec * (ALP_HEADER + 8) + n * 8 + n * 10;
    zb_buf_reset(out);
    if (zb_put_zeros(out, cap)) return rdz_memory(e, "a numeric block");
    for (i = 0; i < n; i += RDZ_ALP_VECTOR) {
        size_t len = n - i < RDZ_ALP_VECTOR ? n - i : RDZ_ALP_VECTOR;
        at += alp_vector_encode(v + i, len, cand, ncand, out->data + at);
        if (at >= limit) return 0;
    }
    out->len = at;
    *used = 1;
    return 0;
}

/* ---- decoding ---------------------------------------------------------------------- */

/* code i of `width` bits from a packed stream of plen bytes */
static uint64_t alp_unpack(const uint8_t *src, size_t plen, size_t i, unsigned w, uint64_t mask)
{
    size_t bit = i * w, byte = bit / 8;
    unsigned shift = (unsigned)(bit % 8), need = (shift + w + 7) / 8, k;
    uint64_t lo, hi = 0;
    if (byte + 8 <= plen) {
        lo = zb_rd_u64le(src + byte);
        if (need > 8) hi = src[byte + 8];
    } else {
        lo = 0;
        for (k = 0; k < need && k < 8; k++) lo |= (uint64_t)src[byte + k] << (8 * k);
        if (need > 8) hi = src[byte + 8];
    }
    return ((lo >> shift) | (shift ? hi << (64 - shift) : 0)) & mask;
}

int rdz_alp_length_ok(uint64_t n, uint64_t len)
{
    uint64_t nvec = (n + RDZ_ALP_VECTOR - 1) / RDZ_ALP_VECTOR;
    return n != 0 && len >= nvec * ALP_HEADER && len <= nvec * (ALP_HEADER + 8) + n * 18;
}

int rdz_alp_decode(const uint8_t *enc, size_t len, size_t n, double *out, rdz_error *e)
{
    size_t at = 0, v;
    if (!rdz_alp_length_ok(n, len)) return rdz_invalid(e, "decimal double block length mismatch");
    for (v = 0; v < n; v += RDZ_ALP_VECTOR) {
        size_t cnt = n - v < RDZ_ALP_VECTOR ? n - v : RDZ_ALP_VECTOR, ncodes, plen, exceptions, i;
        unsigned ee, f, w, flags;
        uint64_t base, mask, acc;
        int64_t limit, k;
        const uint8_t *p;
        int bad = 0;
        double *x = out + v;
        if (len - at < ALP_HEADER) return rdz_invalid(e, "truncated decimal double vector");
        p = enc + at;
        ee = p[0];
        f = p[1];
        w = p[2];
        flags = p[3];
        exceptions = zb_rd_u16le(p + 4);
        base = zb_rd_u64le(p + 8);
        if (ee > ALP_MAX_E || f > ee || w > 64 || flags > ALP_DELTA || zb_rd_u16le(p + 6) ||
            exceptions > cnt || (flags == ALP_DELTA && cnt < 2)) {
            return rdz_invalid(e, "invalid decimal double vector");
        }
        at += ALP_HEADER;
        acc = 0;
        if (flags == ALP_DELTA) {
            if (len - at < 8) return rdz_invalid(e, "truncated decimal double vector");
            acc = zb_rd_u64le(enc + at);
            at += 8;
        }
        ncodes = flags == ALP_DELTA ? cnt - 1 : cnt;
        plen = (ncodes * w + 7) / 8;
        if (len - at < plen || (len - at - plen) / 10 < exceptions) {
            return rdz_invalid(e, "truncated decimal double vector");
        }
        if ((ncodes * w) % 8 && (enc[at + plen - 1] >> ((ncodes * w) % 8))) {
            return rdz_invalid(e, "decimal double vector has nonzero padding bits");
        }
        mask = w == 64 ? ~(uint64_t)0 : ((uint64_t)1 << w) - 1;
        limit = LIMIT[f];
        p = enc + at;
        /* every n within |n * 10^f| < 2^53 (branchless; checked once) */
        if (flags == ALP_DELTA) {
            k = signed_of(acc);
            bad |= k > limit || k < -limit;
            x[0] = alp_value((bad ? 0 : k) * I10[f], (int)ee);
            for (i = 1; i < cnt; i++) {
                acc += base + (w ? alp_unpack(p, plen, i - 1, w, mask) : 0);
                k = signed_of(acc);
                bad |= k > limit || k < -limit;
                x[i] = alp_value((bad ? 0 : k) * I10[f], (int)ee);
            }
        } else {
            for (i = 0; i < cnt; i++) {
                k = signed_of(base + (w ? alp_unpack(p, plen, i, w, mask) : 0));
                bad |= k > limit || k < -limit;
                x[i] = alp_value((bad ? 0 : k) * I10[f], (int)ee);
            }
        }
        if (bad) return rdz_invalid(e, "decimal double out of range");
        at += plen;
        /* exceptions: increasing positions within the vector */
        {
            const uint8_t *pos = enc + at, *val = enc + at + 2 * exceptions;
            size_t prev = 0;
            for (i = 0; i < exceptions; i++) {
                size_t q = zb_rd_u16le(pos + 2 * i);
                if (q >= cnt || (i && q <= prev)) { /* GUARD: alp-exception-position */
                    return rdz_invalid(e, "invalid decimal double exception position");
                }
                x[q] = double_of(zb_rd_u64le(val + 8 * i));
                prev = q;
            }
            at += 10 * exceptions;
        }
    }
    if (at != len) return rdz_invalid(e, "decimal double block has trailing bytes");
    return 0;
}
