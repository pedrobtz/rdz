#include <string.h>

#include <zubin/rw.h>

#include "rdz_numeric.h"

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

/* LSB-first bit packing: code i occupies bits [i * width, (i + 1) * width)
   of a little-endian bit stream, written into zeroed memory. */
typedef struct {
    uint8_t *dst;
    uint64_t acc;
    unsigned have, width;
} rdz_packer;

static void packer_put(rdz_packer *p, uint32_t code)
{
    if (!p->width) return;
    p->acc |= (uint64_t)code << p->have;
    p->have += p->width;
    while (p->have >= 8) {
        *p->dst++ = (uint8_t)p->acc;
        p->acc >>= 8;
        p->have -= 8;
    }
}

static void packer_end(rdz_packer *p)
{
    if (p->have) *p->dst = (uint8_t)p->acc;
}

/* The i-th code of a packed stream of `width` bits, `len` bytes long. A
   code spans at most five bytes; an eight-byte window is read whenever it
   fits. */
static uint32_t unpack_at(const uint8_t *src, size_t len, size_t i, unsigned width,
                          uint64_t mask)
{
    size_t bit = i * width, byte = bit / 8;
    unsigned shift = (unsigned)(bit % 8), k, need;
    uint64_t acc = 0;
    if (byte + 8 <= len) {
        acc = zb_rd_u64le(src + byte);
    } else {
        need = (shift + width + 7) / 8;
        for (k = 0; k < need; k++) acc |= (uint64_t)src[byte + k] << (8 * k);
    }
    return (uint32_t)((acc >> shift) & mask);
}

/* Unused high bits of the last packed byte must be zero (canonical). */
static int padding_clear(const uint8_t *src, size_t n, unsigned width)
{
    size_t bits = n * width;
    if (bits % 8 == 0) return 1;
    return (src[bits / 8] >> (bits % 8)) == 0;
}

static void shuffle(const uint8_t *src, size_t n, size_t width, uint8_t *dst)
{
    size_t i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < width; k++) dst[k * n + i] = src[i * width + k];
    }
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

int rdz_int_encode(const int32_t *v, size_t n, int compressing, zb_buf *out, uint16_t *encoding,
                   rdz_error *e)
{
    int64_t lo = INT32_MAX, hi = INT32_MIN, dlo = INT64_MAX, dhi = INT64_MIN;
    size_t i, runs = n ? 1 : 0, nas = 0;
    size_t raw_len = 4 * n, for_len = SIZE_MAX, delta_len = SIZE_MAX, runs_len;
    unsigned for_width = 0, delta_width = 0;
    uint8_t *dst;

    for (i = 0; i < n; i++) {
        if (v[i] == INT_NA) nas++;
        else {
            if (v[i] < lo) lo = v[i];
            if (v[i] > hi) hi = v[i];
        }
        if (i) {
            if (v[i] != v[i - 1]) runs++;
            if (!nas) {
                int64_t d = (int64_t)v[i] - (int64_t)v[i - 1];
                if (d < dlo) dlo = d;
                if (d > dhi) dhi = d;
            }
        }
    }
    runs_len = RDZ_RUNS_HEADER + RDZ_INT_RUN_RECORD * runs;
    if (n) {
        uint64_t range = nas == n ? 0 : (uint64_t)(hi - lo);
        for_width = bit_length(nas ? range + 1 : range);
        for_len = RDZ_INT_FOR_HEADER + packed_bytes(n, for_width);
    }
    if (n >= 2 && nas == 0) {
        delta_width = bit_length((uint64_t)(dhi - dlo));
        if (delta_width <= 32) delta_len = RDZ_INT_DELTA_HEADER + packed_bytes(n - 1, delta_width);
    }

    /* the smallest; ties go to the cheaper decoder: runs, delta, FOR, raw */
    if (n && runs_len <= delta_len && runs_len <= for_len && runs_len <= raw_len) {
        size_t at = RDZ_RUNS_HEADER;
        if (!(dst = reserve(out, runs_len, e))) return 1;
        zb_wr_u32le(dst, (uint32_t)runs);
        for (i = 1; i <= n; i++) {
            if (i == n || v[i] != v[i - 1]) {
                zb_wr_u32le(dst + at, (uint32_t)v[i - 1]);
                zb_wr_u32le(dst + at + 4, (uint32_t)i);
                at += RDZ_INT_RUN_RECORD;
            }
        }
        *encoding = RDZ_ENCODING_INT_RUNS;
        return 0;
    }
    if (delta_len < for_len && delta_len < raw_len) {
        rdz_packer pk;
        if (!(dst = reserve(out, delta_len, e))) return 1;
        dst[0] = (uint8_t)delta_width;
        zb_wr_u32le(dst + 4, (uint32_t)v[0]);
        zb_wr_u64le(dst + 8, (uint64_t)dlo);
        pk.dst = dst + RDZ_INT_DELTA_HEADER;
        pk.acc = 0;
        pk.have = 0;
        pk.width = delta_width;
        for (i = 1; i < n; i++) packer_put(&pk, (uint32_t)((int64_t)v[i] - (int64_t)v[i - 1] - dlo));
        packer_end(&pk);
        *encoding = RDZ_ENCODING_INT_DELTA;
        return 0;
    }
    if (n && for_len < raw_len) {
        uint32_t na_code = for_width == 32 ? 0xffffffffu : (uint32_t)((1ull << for_width) - 1);
        int32_t base = nas == n ? 0 : (int32_t)lo;
        rdz_packer pk;
        if (!(dst = reserve(out, for_len, e))) return 1;
        dst[0] = (uint8_t)for_width;
        dst[1] = nas ? 1 : 0;
        zb_wr_u32le(dst + 4, (uint32_t)base);
        pk.dst = dst + RDZ_INT_FOR_HEADER;
        pk.acc = 0;
        pk.have = 0;
        pk.width = for_width;
        for (i = 0; i < n; i++) {
            packer_put(&pk, v[i] == INT_NA ? na_code : (uint32_t)((int64_t)v[i] - base));
        }
        packer_end(&pk);
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
    if (!rdz_int_length_ok(encoding, n, len) && !(n == 0 && len == 0)) {
        return rdz_invalid(e, "integer block length mismatch");
    }
    switch (encoding) {
    case RDZ_ENCODING_INT_RAW:
        for (i = 0; i < n; i++) out[i] = (int32_t)zb_rd_u32le(enc + 4 * i);
        return 0;
    case RDZ_ENCODING_INT_SHUFFLE:
        for (i = 0; i < n; i++) {
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
            /* branchless: NA codes are common and unpredictable */
            const uint8_t *src = enc + RDZ_INT_FOR_HEADER;
            size_t plen = len - RDZ_INT_FOR_HEADER;
            uint64_t mask = width == 32 ? 0xffffffffull : ((1ull << width) - 1);
            int bad = 0;
            for (i = 0; i < n; i++) {
                uint32_t c = width ? unpack_at(src, plen, i, width, mask) : 0;
                int na = has_na && c == na_code;
                int64_t value = base + (int64_t)c;
                bad |= !na & (value <= INT32_MIN || value > INT32_MAX);
                out[i] = na ? INT_NA : (int32_t)value;
            }
            if (bad) return bad_int(e);
        }
        return 0;
    }
    case RDZ_ENCODING_INT_DELTA: {
        unsigned width = enc[0];
        int64_t value = (int32_t)zb_rd_u32le(enc + 4), dmin = (int64_t)zb_rd_u64le(enc + 8);
        if (width > 32 || enc[1] || enc[2] || enc[3] || value == INT32_MIN ||
            dmin < -(int64_t)UINT32_MAX || dmin > (int64_t)UINT32_MAX ||
            len != RDZ_INT_DELTA_HEADER + packed_bytes(n - 1, width) ||
            !padding_clear(enc + RDZ_INT_DELTA_HEADER, n - 1, width)) {
            return bad_int(e);
        }
        out[0] = (int32_t)value;
        {
            const uint8_t *src = enc + RDZ_INT_DELTA_HEADER;
            size_t plen = len - RDZ_INT_DELTA_HEADER;
            uint64_t mask = width == 32 ? 0xffffffffull : ((1ull << width) - 1);
            int bad = 0;
            for (i = 1; i < n; i++) {
                uint32_t c = width ? unpack_at(src, plen, i - 1, width, mask) : 0;
                value += dmin + (int64_t)c;
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
            if (end <= start || end > n || (r && value == out[start - 1])) return bad_int(e);
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
    if (!rdz_dbl_length_ok(encoding, n, len) && !(n == 0 && len == 0)) {
        return rdz_invalid(e, "double block length mismatch");
    }
    switch (encoding) {
    case RDZ_ENCODING_DBL_RAW:
        for (i = 0; i < n; i++) out[i] = double_of(zb_rd_u64le(enc + 8 * i));
        return 0;
    case RDZ_ENCODING_DBL_SHUFFLE:
        for (i = 0; i < n; i++) {
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
    default:
        return rdz_invalid(e, "unsupported double block encoding");
    }
}
