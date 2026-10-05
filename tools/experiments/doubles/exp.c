/*
 * Double-encoding experiment for rdz: today's records (plain or byte-shuffled
 * raw + zstd) against ALP (scaled decimals -> integers, frame of reference,
 * bit-packed, exact exceptions; delta variant) and ALP-RD (dictionary of the
 * high bits, bit-packed low bits), with and without zstd on top. Per rdz
 * block of 131,072 values; vectors of 1,024. Bit-exact round trip checked.
 *
 *   exp FILE...
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zstd.h"

#define BLOCK 131072
#define VEC 1024
#define MAXE 18

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint64_t bits(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static double dbl(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }

static const double P10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                             1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
static const double N10[] = {1e0, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9, 1e-10,
                             1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18};
static const double SWEET = 6755399441055744.0; /* 2^52 + 2^51 */

/* x -> integer for (e, f); 0 and *ok = 0 when it does not round-trip */
static int DIV = 0; /* the experiment's decode mode: x = n * 10^f / 10^e */

static inline int64_t alp_enc(double x, int e, int f, int *ok)
{
    double t = x * P10[e] * N10[f];
    int64_t n;
    if (!(t > -2251799813685248.0 && t < 2251799813685248.0)) { *ok = 0; return 0; }
    n = (int64_t)((t + SWEET) - SWEET);
    *ok = bits(DIV ? (double)n * P10[f] / P10[e] : (double)n * P10[f] * N10[e]) == bits(x);
    return n;
}

static inline double alp_dec(int64_t n, int e, int f) { return (double)n * P10[f] * N10[e]; }
static inline double alp_dec_div(int64_t n, int e, int f) { return (double)n * P10[f] / P10[e]; }

static int width_of(uint64_t range)
{
    int w = 0;
    while (w < 64 && (range >> w)) w++;
    return w;
}

/* ---- bit packing (LSB first, 64-bit words) ---- */

static size_t pack(const uint64_t *v, size_t n, int w, uint8_t *out)
{
    size_t bytes = (n * (size_t)w + 7) / 8, i;
    uint64_t acc = 0;
    int have = 0;
    uint8_t *p = out;
    if (w == 0) return 0;
    for (i = 0; i < n; i++) {
        uint64_t x = v[i];
        acc |= x << have;
        if (have + w >= 64) {
            memcpy(p, &acc, 8);
            p += 8;
            acc = have ? x >> (64 - have) : 0;
            have = have + w - 64;
        } else {
            have += w;
        }
    }
    if (have) memcpy(p, &acc, (have + 7) / 8);
    return bytes;
}

static void unpack(const uint8_t *in, size_t n, int w, uint64_t *v)
{
    size_t i, bit = 0;
    uint64_t mask = w == 64 ? ~0ull : ((1ull << w) - 1);
    if (w == 0) { memset(v, 0, n * 8); return; }
    for (i = 0; i < n; i++, bit += (size_t)w) {
        size_t byte = bit >> 3;
        int sh = (int)(bit & 7);
        uint64_t lo, hi = 0;
        memcpy(&lo, in + byte, 8);
        if (sh + w > 64) hi = (uint64_t)in[byte + 8] << (64 - sh);
        v[i] = ((lo >> sh) | hi) & mask;
    }
}

/* ---- ALP per vector ---- */

typedef struct { int e, f; } combo;

/* estimated bytes for a sample under (e, f) */
static double alp_estimate(const double *x, size_t n, int e, int f)
{
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    size_t i, exc = 0;
    for (i = 0; i < n; i++) {
        int ok;
        int64_t v = alp_enc(x[i], e, f, &ok);
        if (!ok) { exc++; continue; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (exc == n) return 1e30;
    return n * width_of((uint64_t)(hi - lo)) / 8.0 + exc * 10.0;
}

/* the block's candidate combos: most frequent best combos over sampled vectors */
static int alp_candidates(const double *x, size_t n, combo *out, int k)
{
    int count[MAXE + 1][MAXE + 1];
    size_t nvec = (n + VEC - 1) / VEC, step = nvec > 8 ? nvec / 8 : 1, v;
    int ncand = 0, e, f;
    memset(count, 0, sizeof count);
    for (v = 0; v < nvec; v += step) {
        double s[32];
        size_t len = v * VEC + VEC <= n ? VEC : n - v * VEC, m = 0, i;
        double best = 1e31;
        int be = 0, bf = 0;
        for (i = 0; i < len && m < 32; i += len / 32 ? len / 32 : 1) s[m++] = x[v * VEC + i];
        for (e = MAXE; e >= 0; e--) {
            for (f = e; f >= 0; f--) {
                double c = alp_estimate(s, m, e, f);
                if (c < best || (c == best && e - f < be - bf)) { best = c; be = e; bf = f; }
            }
        }
        count[be][bf]++;
    }
    while (ncand < k) {
        int bc = 0, be = -1, bf = -1;
        for (e = 0; e <= MAXE; e++) for (f = 0; f <= e; f++) if (count[e][f] > bc) { bc = count[e][f]; be = e; bf = f; }
        if (be < 0) break;
        out[ncand].e = be; out[ncand].f = bf; ncand++;
        count[be][bf] = 0;
    }
    return ncand;
}

/* one vector: header e f w delta | u16 exc | i64 base | packed | exc positions u16 | exc values u64 */
static size_t alp_vec_encode(const double *x, size_t n, const combo *cand, int ncand, int allow_delta,
                             uint8_t *out)
{
    int64_t ints[VEC];
    uint64_t u[VEC];
    uint16_t pos[VEC];
    size_t i, exc = 0, at = 0;
    int c, best = 0, e, f, w, delta = 0;
    int64_t lo = INT64_MAX, hi = INT64_MIN, fill = 0, base;
    if (ncand > 1) {
        double s[32], bc = 1e31;
        size_t m = 0;
        for (i = 0; i < n && m < 32; i += n / 32 ? n / 32 : 1) s[m++] = x[i];
        for (c = 0; c < ncand; c++) {
            double est = alp_estimate(s, m, cand[c].e, cand[c].f);
            if (est < bc) { bc = est; best = c; }
        }
    }
    e = cand[best].e; f = cand[best].f;
    for (i = 0; i < n; i++) {
        int ok;
        ints[i] = alp_enc(x[i], e, f, &ok);
        if (!ok) pos[exc++] = (uint16_t)i;
        else { fill = ints[i]; }
    }
    /* exceptions take a neighbour's value so they do not widen the range */
    {
        size_t k;
        int64_t last = fill;
        for (k = 0, i = 0; i < n; i++) {
            if (k < exc && pos[k] == i) { ints[i] = last; k++; }
            else last = ints[i];
        }
    }
    for (i = 0; i < n; i++) { if (ints[i] < lo) lo = ints[i]; if (ints[i] > hi) hi = ints[i]; }
    w = width_of((uint64_t)(hi - lo));
    base = lo;
    if (allow_delta && n > 1) {
        int64_t dlo = INT64_MAX, dhi = INT64_MIN, d;
        int dw;
        int overflow = 0;
        for (i = 1; i < n; i++) {
            if (__builtin_sub_overflow(ints[i], ints[i - 1], &d)) { overflow = 1; break; }
            if (d < dlo) dlo = d;
            if (d > dhi) dhi = d;
        }
        if (!overflow) {
            dw = width_of((uint64_t)(dhi - dlo));
            if (dw + 1 < w) {
                delta = 1; w = dw; base = dlo;
                for (i = n - 1; i > 0; i--) u[i] = (uint64_t)(ints[i] - ints[i - 1] - dlo);
                u[0] = 0;
            }
        }
    }
    if (!delta) for (i = 0; i < n; i++) u[i] = (uint64_t)(ints[i] - base);
    out[at++] = (uint8_t)e; out[at++] = (uint8_t)f; out[at++] = (uint8_t)w; out[at++] = (uint8_t)delta;
    memcpy(out + at, &exc, 2); at += 2;
    memcpy(out + at, &base, 8); at += 8;
    if (delta) { memcpy(out + at, &ints[0], 8); at += 8; }
    at += pack(u, n, w, out + at);
    memcpy(out + at, pos, exc * 2); at += exc * 2;
    for (i = 0; i < exc; i++) { uint64_t b = bits(x[pos[i]]); memcpy(out + at, &b, 8); at += 8; }
    return at;
}

static size_t alp_vec_decode(const uint8_t *in, size_t n, double *x)
{
    uint64_t u[VEC + 1];
    size_t at = 0, i, exc = 0;
    int e = in[0], f = in[1], w = in[2], delta = in[3];
    int64_t base, first = 0;
    at = 4;
    memcpy(&exc, in + at, 2); exc &= 0xffff; at += 2;
    memcpy(&base, in + at, 8); at += 8;
    if (delta) { memcpy(&first, in + at, 8); at += 8; }
    unpack(in + at, n, w, u);
    at += (n * (size_t)w + 7) / 8;
    if (delta) {
        int64_t v = first;
        for (i = 1; i < n; i++) { v += (int64_t)u[i] + base; u[i] = (uint64_t)v; }
        u[0] = (uint64_t)first;
        if (DIV) for (i = 0; i < n; i++) x[i] = alp_dec_div((int64_t)u[i], e, f);
        else for (i = 0; i < n; i++) x[i] = alp_dec((int64_t)u[i], e, f);
    } else if (DIV) {
        for (i = 0; i < n; i++) x[i] = alp_dec_div((int64_t)u[i] + base, e, f);
    } else {
        for (i = 0; i < n; i++) x[i] = alp_dec((int64_t)u[i] + base, e, f);
    }
    {
        const uint8_t *pp = in + at, *vp = in + at + exc * 2;
        for (i = 0; i < exc; i++) {
            uint16_t p; uint64_t b;
            memcpy(&p, pp + 2 * i, 2); memcpy(&b, vp + 8 * i, 8);
            x[p] = dbl(b);
        }
        at += exc * 10;
    }
    return at;
}

/* ---- ALP-RD per block: left bits through a dictionary of 8, right bits packed ---- */

static size_t rd_encode(const double *x, size_t n, uint8_t *out)
{
    int r, bestr = 48, d;
    double best = 1e31;
    uint16_t dict[8];
    size_t at = 0, i, exc = 0;
    static uint64_t right[BLOCK], code[BLOCK];
    /* choose the split on a sample */
    for (r = 48; r <= 63; r++) {
        /* count the 8 most frequent left values in a sample */
        uint16_t lv[256]; size_t lc[256]; int nl = 0, j;
        size_t m = 0, covered = 0, s;
        for (s = 0; s < n; s += n / 256 ? n / 256 : 1, m++) {
            uint16_t l = (uint16_t)(bits(x[s]) >> r);
            for (j = 0; j < nl && lv[j] != l; j++) ;
            if (j == nl && nl < 256) { lv[nl] = l; lc[nl++] = 0; }
            if (j < 256) lc[j]++;
        }
        for (d = 0; d < 8 && d < nl; d++) {
            int bj = d;
            for (j = d + 1; j < nl; j++) if (lc[j] > lc[bj]) bj = j;
            { uint16_t tl = lv[d]; size_t tc = lc[d]; lv[d] = lv[bj]; lc[d] = lc[bj]; lv[bj] = tl; lc[bj] = tc; }
            covered += lc[d];
        }
        {
            double cost = m * (r + 3) / 8.0 + (m - covered) * 4.0;
            if (cost < best) { best = cost; bestr = r; }
        }
    }
    r = bestr;
    /* the dictionary over the whole block */
    {
        uint16_t lv[4096]; size_t lc[4096]; int nl = 0, j;
        for (i = 0; i < n; i++) {
            uint16_t l = (uint16_t)(bits(x[i]) >> r);
            for (j = 0; j < nl && lv[j] != l; j++) ;
            if (j == nl) { if (nl == 4096) continue; lv[nl] = l; lc[nl++] = 0; }
            lc[j]++;
        }
        for (d = 0; d < 8; d++) {
            int bj = d;
            if (d >= nl) { dict[d] = 0; continue; }
            for (j = d + 1; j < nl; j++) if (lc[j] > lc[bj]) bj = j;
            { uint16_t tl = lv[d]; size_t tc = lc[d]; lv[d] = lv[bj]; lc[d] = lc[bj]; lv[bj] = tl; lc[bj] = tc; }
            dict[d] = lv[d];
        }
    }
    for (i = 0; i < n; i++) {
        uint64_t b = bits(x[i]);
        uint16_t l = (uint16_t)(b >> r);
        for (d = 0; d < 8 && dict[d] != l; d++) ;
        right[i] = b & ((1ull << r) - 1);
        code[i] = d == 8 ? 0 : (uint64_t)d;
        if (d == 8) exc++;
    }
    /* header: r, dict[8], exc count u32; codes(3 bits); rights(r bits); exceptions (u32 pos, u16 left) */
    out[at++] = (uint8_t)r;
    memcpy(out + at, dict, 16); at += 16;
    {
        uint32_t ec = 0;
        size_t ecat;
        ecat = at; at += 4;
        at += pack(code, n, 3, out + at);
        at += pack(right, n, r, out + at);
        for (i = 0; i < n; i++) {
            uint64_t b = bits(x[i]);
            uint16_t l = (uint16_t)(b >> r);
            for (d = 0; d < 8 && dict[d] != l; d++) ;
            if (d == 8) {
                uint32_t p = (uint32_t)i;
                memcpy(out + at, &p, 4); at += 4;
                memcpy(out + at, &l, 2); at += 2;
                ec++;
            }
        }
        memcpy(out + ecat, &ec, 4);
    }
    (void)exc;
    return at;
}

static size_t rd_decode(const uint8_t *in, size_t n, double *x)
{
    static uint64_t right[BLOCK], code[BLOCK];
    uint16_t dict[8];
    size_t at = 0, i;
    uint32_t ec;
    int r = in[at++];
    memcpy(dict, in + at, 16); at += 16;
    memcpy(&ec, in + at, 4); at += 4;
    unpack(in + at, n, 3, code); at += (n * 3 + 7) / 8;
    unpack(in + at, n, r, right); at += (n * (size_t)r + 7) / 8;
    for (i = 0; i < n; i++) x[i] = dbl(((uint64_t)dict[code[i]] << r) | right[i]);
    for (i = 0; i < ec; i++) {
        uint32_t p; uint16_t l;
        memcpy(&p, in + at, 4); memcpy(&l, in + at + 4, 2); at += 6;
        x[p] = dbl(((uint64_t)l << r) | right[p]);
    }
    return at;
}

/* ---- a block: ALP or ALP-RD, chosen on a sample; 1 byte scheme ---- */

static size_t block_encode(const double *x, size_t n, uint8_t *out, int allow_delta, int *scheme)
{
    combo cand[5];
    int k = alp_candidates(x, n, cand, 5);
    size_t at = 1, v;
    /* ALP's estimate from the sample vs RD: pick ALP when it beats ~RD's 52-56 bits */
    double s[256], est;
    size_t m = 0, i;
    for (i = 0; i < n && m < 256; i += n / 256 ? n / 256 : 1) s[m++] = x[i];
    est = k ? alp_estimate(s, m, cand[0].e, cand[0].f) * 8.0 / m : 1e9;
    if (k && est < 48.0) {
        out[0] = 0;
        *scheme = 0;
        for (v = 0; v < n; v += VEC) {
            size_t len = n - v < VEC ? n - v : VEC;
            at += alp_vec_encode(x + v, len, cand, k, allow_delta, out + at);
        }
        return at;
    }
    out[0] = 1;
    *scheme = 1;
    return 1 + rd_encode(x, n, out + 1);
}

static size_t block_decode(const uint8_t *in, size_t n, double *x)
{
    size_t at = 1, v;
    if (in[0] == 1) return 1 + rd_decode(in + 1, n, x);
    for (v = 0; v < n; v += VEC) {
        size_t len = n - v < VEC ? n - v : VEC;
        at += alp_vec_decode(in + at, len, x + v);
    }
    return at;
}

/* ---- baselines ---- */

static void shuffle(const double *x, size_t n, uint8_t *out)
{
    const uint8_t *b = (const uint8_t *)x;
    size_t i; int k;
    for (i = 0; i < n; i++) for (k = 0; k < 8; k++) out[k * n + i] = b[8 * i + k];
}

static void unshuffle(const uint8_t *in, size_t n, double *x)
{
    uint8_t *b = (uint8_t *)x;
    size_t i; int k;
    for (i = 0; i < n; i++) for (k = 0; k < 8; k++) b[8 * i + k] = in[k * n + i];
}

typedef struct { const char *name; double enc_s, dec_s; size_t bytes; int exact; } result;

enum { RAW, PLAIN_Z, SHUF_Z, ALP, ALP_D, ALP_Z, NMETH };
static const char *names[NMETH] = {"raw", "plain+zstd", "shuffle+zstd", "alp", "alp+delta", "alp+delta+zstd"};

int main(int argc, char **argv)
{
    int a;
    if (getenv("ALP_DIV")) DIV = 1;
    printf("%-12s %-15s %8s %9s %9s %s\n", "data", "method", "bits/val", "enc MB/s", "dec MB/s", "exact");
    for (a = 1; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        size_t n, b, cap;
        double *x, *y;
        uint8_t *buf, *tmp;
        int m;
        char label[64];
        const char *base = strrchr(argv[a], '/');
        snprintf(label, sizeof label, "%s", base ? base + 1 : argv[a]);
        *strrchr(label, '.') = 0;
        fseek(fp, 0, SEEK_END); n = (size_t)ftell(fp) / 8; fseek(fp, 0, SEEK_SET);
        x = malloc(n * 8); y = malloc(n * 8);
        if (fread(x, 8, n, fp) != n) return 1;
        fclose(fp);
        cap = ZSTD_compressBound(BLOCK * 8) + BLOCK * 16 + 4096;
        buf = malloc(cap * ((n + BLOCK - 1) / BLOCK)); tmp = malloc(cap);
        for (m = 0; m < NMETH; m++) {
            double te = 1e9, td = 1e9;
            size_t total = 0;
            int rep, ralp = 0, rrd = 0;
            for (rep = 0; rep < 3; rep++) {
                size_t at = 0;
                size_t *sizes = malloc(sizeof(size_t) * (n / BLOCK + 2)), nb = 0;
                double t0 = now(), t1, t2;
                for (b = 0; b < n; b += BLOCK) {
                    size_t len = n - b < BLOCK ? n - b : BLOCK, s;
                    int sch = 0;
                    switch (m) {
                    case RAW: memcpy(buf + at, x + b, len * 8); s = len * 8; break;
                    case PLAIN_Z: s = ZSTD_compress(buf + at, cap, x + b, len * 8, 1); break;
                    case SHUF_Z: shuffle(x + b, len, tmp); s = ZSTD_compress(buf + at, cap, tmp, len * 8, 1); break;
                    case ALP: s = block_encode(x + b, len, buf + at, 0, &sch); break;
                    case ALP_D: s = block_encode(x + b, len, buf + at, 1, &sch); break;
                    default: {
                        size_t r = block_encode(x + b, len, tmp, 1, &sch);
                        s = ZSTD_compress(buf + at, cap, tmp, r, 1);
                        if (s >= r) { memcpy(buf + at, tmp, r); s = r | ((size_t)1 << 62); }
                    }
                    }
                    if (rep == 0) { if (sch) rrd++; else ralp++; }
                    sizes[nb++] = s; at += s & ~((size_t)1 << 62);
                }
                t1 = now();
                at = 0; nb = 0;
                for (b = 0; b < n; b += BLOCK) {
                    size_t len = n - b < BLOCK ? n - b : BLOCK, s = sizes[nb++];
                    size_t sz = s & ~((size_t)1 << 62);
                    switch (m) {
                    case RAW: memcpy(y + b, buf + at, len * 8); break;
                    case PLAIN_Z: ZSTD_decompress(y + b, len * 8, buf + at, sz); break;
                    case SHUF_Z: ZSTD_decompress(tmp, len * 8, buf + at, sz); unshuffle(tmp, len, y + b); break;
                    case ALP: case ALP_D: block_decode(buf + at, len, y + b); break;
                    default:
                        if (s >> 62) block_decode(buf + at, len, y + b);
                        else { ZSTD_decompress(tmp, cap, buf + at, sz); block_decode(tmp, len, y + b); }
                    }
                    at += sz;
                }
                t2 = now();
                total = at;
                if (t1 - t0 < te) te = t1 - t0;
                if (t2 - t1 < td) td = t2 - t1;
                free(sizes);
            }
            {
                int exact = memcmp(x, y, n * 8) == 0;
                char nm[64];
                snprintf(nm, sizeof nm, "%s", names[m]);
                if (m >= ALP) snprintf(nm, sizeof nm, "%s%s", names[m], rrd ? (ralp ? " (mix)" : " (rd)") : "");
                printf("%-12s %-15s %8.2f %9.0f %9.0f %s\n", label, nm, total * 8.0 / n,
                       n * 8 / te / 1e6, n * 8 / td / 1e6, exact ? "yes" : "NO");
            }
        }
        free(x); free(y); free(buf); free(tmp);
        printf("\n");
    }
    return 0;
}
