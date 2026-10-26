/*
 * fp.c: correctly rounded fma (f32, f64) and ieee half conversions, round to nearest even.
 * these back vfma / vfnms / vcvtb. checked against qemu's cortex-m7 on edge and random
 * vectors (test/fpvec). compile with -fno-fast-math -ffp-contract=off.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "rt.h"

uint32_t fma32_exact(uint32_t a, uint32_t b, uint32_t c)
{
    double p = (double)f32(a) * (double)f32(b);      /* exact: 24 + 24 bits < 53 */
    double z = (double)f32(c);
    double s = p + z;
    if (s - s != 0.0) return b32((float)s);           /* inf or nan: no rounding issue */
    double bb = s - p, e = (p - (s - bb)) + (z - bb); /* twosum: s + e == p + z exactly */
    if (e != 0.0) {
        uint64_t sb = b64(s);
        if (!(sb & 1u)) sb += ((e > 0) == (s > 0)) ? 1 : (uint64_t)-1;   /* round to odd */
        s = f64(sb);
    }
    return b32((float)s);
}

/* ------------------------------------------------------------- f64 fma */
typedef unsigned __int128 u128;
static int clz128(u128 x)
{
    uint64_t hi = (uint64_t)(x >> 64);
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)x);
}
uint64_t fma64_exact(uint64_t a, uint64_t b, uint64_t c) { int t; return fma64_exact_t(a, b, c, &t); }
/* *tiny: the exact (unrounded) result is nonzero and below 2^-1022 (for FPSCR.FZ) */
uint64_t fma64_exact_t(uint64_t a, uint64_t b, uint64_t c, int *tiny)
{
    *tiny = 0;
    const uint64_t EM = 0x7ff, MM = (1ull << 52) - 1;
    int ea = (int)((a >> 52) & EM), eb = (int)((b >> 52) & EM), ec = (int)((c >> 52) & EM);
    /* inf / nan operands, by sign rules on the exact product (which is never computed: a
     * finite product can exceed DBL_MAX, and finite + inf must still give that inf).
     * nans are sorted out by the caller (fmuladd64); here only return some nan for them. */
    if (ea == 0x7ff || eb == 0x7ff || ec == 0x7ff) {
        if (isnan64(a) || isnan64(b) || isnan64(c)) return DEFNAN64;
        int ainf = ea == 0x7ff, binf = eb == 0x7ff, cinf = ec == 0x7ff;
        int azero = !(a << 1), bzero = !(b << 1);
        uint64_t ps = (a ^ b) & 0x8000000000000000ull;
        if (ainf || binf) {
            if ((ainf && bzero) || (binf && azero)) return DEFNAN64;          /* inf * 0 */
            if (cinf && (c & 0x8000000000000000ull) != ps) return DEFNAN64;   /* inf - inf */
            return ps | 0x7ff0000000000000ull;
        }
        return c;                                                              /* finite + inf */
    }
    uint64_t sp = (a ^ b) >> 63, sc = c >> 63;
    uint64_t ma = a & MM, mb = b & MM, mc = c & MM;
    if (ea) ma |= 1ull << 52; else ea = 1;
    if (eb) mb |= 1ull << 52; else eb = 1;
    if (ec) mc |= 1ull << 52; else ec = 1;
    if (ma == 0 || mb == 0) {                          /* zero product: exact sign rules */
        if (mc == 0) return (sp & sc) ? 0x8000000000000000ull : 0;
        return c;
    }
    /* value = m * 2^(e - 1075). product m has up to 106 bits, exponent ea+eb-2150 */
    u128 mp = (u128)ma * mb;
    int ep = ea + eb - 2150;                            /* product = mp * 2^ep */
    int ecc = ec - 1075;                                /* addend = mc * 2^ecc */
    if (mc == 0) ecc = ep;
    /* bring both to a common exponent with 3 extra low bits room; keep a sticky bit */
    const int G = 3;
    u128 x = mp << G; int ex = ep - G;
    u128 y = (u128)mc << G; int ey = ecc - G;
    /* normalise so that the larger-exponent operand sits high in 128 bits */
    int sh;
    sh = clz128(x) - 2; if (sh > 0) { x <<= sh; ex -= sh; }
    if (mc) { sh = clz128(y) - 2; if (sh > 0) { y <<= sh; ey -= sh; } }
    uint64_t sx = sp, sy = sc;
    if (mc == 0 || ex >= ey) {
        if (mc) {
            int d = ex - ey;
            if (d >= 127) y = y != 0;
            else if (d > 0) y = (y >> d) | ((y & (((u128)1 << d) - 1)) != 0);
        }
    } else {
        int d = ey - ex;
        if (d >= 127) x = x != 0;
        else x = (x >> d) | ((x & (((u128)1 << d) - 1)) != 0);
        ex = ey;
    }
    u128 m; uint64_t s;
    if (mc == 0) { m = x; s = sx; }
    else if (sx == sy) { m = x + y; s = sx; }
    else if (x >= y) { m = x - y; s = sx; }
    else { m = y - x; s = sy; }
    if (m == 0) return 0;                               /* exact cancellation: +0 in rn */
    /* normalise m to have its top bit at position 116 (53 bits + 64 guard/sticky) */
    int top = 127 - clz128(m);
    int e = ex;
    const int T = 116;
    if (top > T) { int d = top - T; m = (m >> d) | ((m & (((u128)1 << d) - 1)) != 0); e += d; }
    else if (top < T) { m <<= (T - top); e -= (T - top); }
    /* now value = m * 2^e with m in [2^116, 2^117). biased exponent of result: */
    int be = e + 116 + 1023;                            /* m = 1.xxx * 2^116 */
    if (be <= 0) *tiny = 1;
    int drop = 64;                                      /* bits below the 53 bit mantissa */
    if (be <= 0) {                                      /* subnormal: drop more bits */
        int extra = 1 - be;
        drop += extra; be = 0;
        if (drop >= 118) return s << 63;                /* m < 2^117 <= half an ulp: rounds to 0 */
    }
    u128 keep = m >> drop;
    u128 rem = m & (((u128)1 << drop) - 1);
    u128 half = (u128)1 << (drop - 1);
    if (rem > half || (rem == half && (keep & 1))) keep++;
    uint64_t mant = (uint64_t)keep;
    if (be == 0) {                                      /* subnormal (may round up to normal) */
        return (s << 63) | mant;                        /* mant < 2^52 -> subnormal, == 2^52 -> min normal */
    }
    if (mant >> 53) { mant >>= 1; be++; }               /* rounding carried to 2^53 */
    if (be >= 0x7ff) return (s << 63) | 0x7ff0000000000000ull;
    return (s << 63) | ((uint64_t)be << 52) | (mant & MM);
}

/* ---------------------------------------------------------------- half */
uint32_t cvt_f32_f16(uint32_t hb)
{
    uint32_t h = hb & 0xffffu, s = (h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0x1f) return m ? (s | 0x7fc00000u | (m << 13)) : (s | 0x7f800000u);
    if (e == 0) {
        if (m == 0) return s;
        int k = 0;
        while (!(m & 0x400)) { m <<= 1; k++; }
        return s | ((uint32_t)(113 - k) << 23) | ((m & 0x3ff) << 13);
    }
    return s | ((e + 112) << 23) | (m << 13);
}
uint32_t cvt_f16_f32(uint32_t a)
{
    uint32_t s = (a >> 16) & 0x8000u, e = (a >> 23) & 0xff, m = a & 0x7fffffu;
    if (e == 0xff) return m ? (s | 0x7e00u | (m >> 13)) : (s | 0x7c00u);
    if (e == 0 && m == 0) return s;
    int ue = (int)e - 127;                              /* unbiased */
    uint32_t mant = e ? (m | 0x800000u) : m;            /* 24 bit significand (f32 subnormal: no implicit 1) */
    if (!e) ue = -126;
    /* half normal: ue in [-14, 15], 10 bit fraction. value = mant * 2^(ue-23) */
    int shift;                                          /* bits to drop from mant */
    uint32_t he;
    if (ue >= -14) { shift = 13; he = (uint32_t)(ue + 15); }
    else { shift = 13 + (-14 - ue); he = 0; }
    if (shift > 31) return s;                           /* far below half subnormal: rounds to 0 */
    uint32_t keep = mant >> shift, rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (keep & 1))) keep++;
    if (he == 0) return s | keep;                       /* subnormal; keep == 0x400 is min normal */
    if (keep >> 11) { keep >>= 1; he++; }
    if (he >= 31) return s | 0x7c00u;
    return s | (he << 10) | (keep & 0x3ff);
}
