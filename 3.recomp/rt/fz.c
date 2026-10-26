/*
 * fz.c: the vfp operations when FPSCR.FZ = 1 (flush to zero), as the armv7-m pseudocode
 * defines them: denormal inputs are read as zero of the same sign (FPUnpack); a result whose
 * exact value is below the smallest normal number, judged before rounding, becomes zero of
 * the result's sign (FPRound). the only boundary that needs more than the rounded result is
 * a result that rounds to exactly the smallest normal: each op checks its exact value there.
 * the firmware sets FPDSCR.FZ, so interrupt handlers run with it. DN, AHP and a non-nearest
 * RMode are never set by this firmware: rt_fz_mode_check stops if they appear.
 * checked against qemu with FZ = 1 in test/fpvec.c.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "rt.h"

#define MIN32 0x00800000u
#define MIN64 0x0010000000000000ull

static inline uint32_t in32(uint32_t a) { return ((a & 0x7f800000u) == 0) ? (a & 0x80000000u) : a; }
static inline uint64_t in64(uint64_t a) { return ((a & 0x7ff0000000000000ull) == 0) ? (a & 0x8000000000000000ull) : a; }
/* rounded result r; tiny tells whether the exact value was below the smallest normal */
static inline uint32_t out32(uint32_t r, int tiny) { return tiny ? (r & 0x80000000u) : r; }
static inline uint64_t out64(uint64_t r, int tiny) { return tiny ? (r & 0x8000000000000000ull) : r; }

void rt_fz_mode_check(void)
{
    if (FPSCR & 0x06C00000u) rt_fatal("fpscr mode not implemented (dn, ahp or rmode set)", FPSCR);
}

/* r was computed from normal-or-zero inputs; exact value tiny? r below MIN: yes; r above:
 * no; r == MIN: decided by the caller's exact comparison (cmp < 0: exact magnitude below) */
static inline int tiny32(uint32_t r, int cmp_at_min) { uint32_t m = r & 0x7fffffffu; if (isnan32(r) || m > MIN32) return 0; if (m < MIN32) return m != 0; return cmp_at_min < 0; }
static inline int tiny64(uint64_t r, int cmp_at_min) { uint64_t m = r & 0x7fffffffffffffffull; if (isnan64(r) || m > MIN64) return 0; if (m < MIN64) return m != 0; return cmp_at_min < 0; }

/* ---------------------------------------------------------------- f32 */
uint32_t fz_add32(uint32_t a, uint32_t b, int sub)
{
    a = in32(a); b = in32(b);
    uint32_t r = sub ? fsub32(a, b) : fadd32(a, b);  /* nans propagate unnegated */
    double e = (double)f32(a) + (sub ? -(double)f32(b) : (double)f32(b));   /* exact near the boundary */
    return out32(r, tiny32(r, e != e ? 0 : (fabs_d(e) < 1.1754943508222875e-38 ? -1 : 0)));
}
uint32_t fz_mul32(uint32_t a, uint32_t b)
{
    a = in32(a); b = in32(b);
    uint32_t r = fmul32(a, b);
    double e = (double)f32(a) * (double)f32(b);      /* exact */
    return out32(r, tiny32(r, fabs_d(e) < 1.1754943508222875e-38 ? -1 : 0));
}
uint32_t fz_div32(uint32_t a, uint32_t b)
{
    a = in32(a); b = in32(b);
    uint32_t r = fdiv32(a, b);
    /* |a/b| < 2^-126  <=>  |a| < |b| * 2^-126 (exact in double) */
    double x = fabs_d((double)f32(a)), y = fabs_d((double)f32(b)) * 1.1754943508222875e-38;
    return out32(r, tiny32(r, x < y ? -1 : 0));
}
uint32_t fz_sqrt32(uint32_t a) { return fsqrt32(in32(a)); }   /* sqrt of a normal is never tiny */
uint32_t fz_fmuladd32(uint32_t addend, uint32_t x, uint32_t y)
{
    addend = in32(addend); x = in32(x); y = in32(y);
    uint32_t r = fmuladd32(addend, x, y);
    /* round-to-odd double of the exact value (as in fma32_exact) never lands on 2^-126 unless exact */
    double p = (double)f32(x) * (double)f32(y), z = (double)f32(addend), s = p + z;
    if (s - s == 0.0) {
        double bb = s - p, e = (p - (s - bb)) + (z - bb);
        if (e != 0.0) { uint64_t sb = b64(s); if (!(sb & 1u)) sb += ((e > 0) == (s > 0)) ? 1 : (uint64_t)-1; s = f64(sb); }
    }
    return out32(r, tiny32(r, fabs_d(s) < 1.1754943508222875e-38 ? -1 : 0));
}
uint32_t fz_cmp32(uint32_t a, uint32_t b) { return fcmp32(in32(a), in32(b)); }
uint32_t fz_maxnm32(uint32_t a, uint32_t b) { return fmaxnm32(in32(a), in32(b)); }
uint32_t fz_minnm32(uint32_t a, uint32_t b) { return fminnm32(in32(a), in32(b)); }
uint32_t fz_rintz32(uint32_t a) { return frintz32(in32(a)); }
uint32_t fz_cvt_s32_f32(uint32_t a) { return cvt_s32_f32(in32(a)); }
uint32_t fz_cvt_u32_f32(uint32_t a) { return cvt_u32_f32(in32(a)); }
uint32_t fz_cvt_s32_f32_fix(uint32_t a, int fb) { return cvt_s32_f32_fix(in32(a), fb); }
uint64_t fz_cvt_f64_f32(uint32_t a) { return cvt_f64_f32(in32(a)); }
uint32_t fz_cvt_f16_f32(uint32_t a) { return cvt_f16_f32(in32(a)); }   /* half output: fz does not apply */

/* ---------------------------------------------------------------- f64 */
static int cmp_exact_mul64(uint64_t a, uint64_t b)
{   /* sign of |a*b| - 2^-1022, exactly, for normal a, b */
    int ea, eb;
    double ma = __builtin_frexp(fabs_d(f64(a)), &ea), mb = __builtin_frexp(fabs_d(f64(b)), &eb);
    int k = -1022 - ea - eb;                          /* |a*b| < 2^-1022 <=> ma*mb < 2^k, ma*mb in [1/4, 1) */
    if (k >= 1) return -1;
    if (k <= -3) return 1;
    double t = __builtin_ldexp(1.0, k);
    uint64_t d = fma64_exact(b64(ma), b64(mb), b64(-t));   /* ma*mb - t, one rounding: sign is exact */
    double dv = f64(d);
    return dv < 0 ? -1 : dv > 0 ? 1 : 0;
}
static int cmp_exact_div64(uint64_t a, uint64_t b)
{   /* sign of |a/b| - 2^-1022 for normal a, b: |a| vs |b|*2^-1022 on mantissas */
    int ea, eb;
    double ma = __builtin_frexp(fabs_d(f64(a)), &ea), mb = __builtin_frexp(fabs_d(f64(b)), &eb);
    int k = -1022 - ea + eb;                          /* |a/b| < 2^-1022 <=> ma < mb * 2^k, ma/mb in (1/2, 2) */
    if (k >= 2) return -1;
    if (k <= -2) return 1;
    double y = __builtin_ldexp(mb, k);
    return ma < y ? -1 : ma > y ? 1 : 0;
}
uint64_t fz_add64(uint64_t a, uint64_t b, int sub)
{
    a = in64(a); b = in64(b);
    uint64_t r = sub ? fsub64(a, b) : fadd64(a, b);  /* nans propagate unnegated */
    double x = f64(a), y = sub ? -f64(b) : f64(b), s = f64(r);
    int c = 0;
    if (s - s == 0.0) { double bb = s - x, e = (x - (s - bb)) + (y - bb); if (e != 0.0) c = ((e > 0) == (s > 0)) ? 1 : -1; }
    return out64(r, tiny64(r, c));
}
uint64_t fz_mul64(uint64_t a, uint64_t b)
{
    a = in64(a); b = in64(b);
    uint64_t r = fmul64(a, b);
    int c = ((a << 1) && (b << 1) && !isnan64(r) && (r & 0x7fffffffffffffffull) == MIN64) ? cmp_exact_mul64(a, b) : 0;
    return out64(r, tiny64(r, c));
}
uint64_t fz_div64(uint64_t a, uint64_t b)
{
    a = in64(a); b = in64(b);
    uint64_t r = fdiv64(a, b);
    int c = ((a << 1) && (b << 1) && !isnan64(r) && (r & 0x7fffffffffffffffull) == MIN64) ? cmp_exact_div64(a, b) : 0;
    return out64(r, tiny64(r, c));
}
uint64_t fz_fmuladd64(uint64_t addend, uint64_t x, uint64_t y)
{
    addend = in64(addend); x = in64(x); y = in64(y);
    int tiny = 0;
    uint64_t r = fmuladd64_t(addend, x, y, &tiny);
    return out64(r, !isnan64(r) && tiny);
}
uint32_t fz_cmp64(uint64_t a, uint64_t b) { return fcmp64(in64(a), in64(b)); }
uint64_t fz_maxnm64(uint64_t a, uint64_t b) { return fmaxnm64(in64(a), in64(b)); }
uint64_t fz_minnm64(uint64_t a, uint64_t b) { return fminnm64(in64(a), in64(b)); }
uint64_t fz_rintz64(uint64_t a) { return frintz64(in64(a)); }
uint32_t fz_cvt_s32_f64(uint64_t a) { return cvt_s32_f64(in64(a)); }
uint32_t fz_cvt_f32_f64(uint64_t a)
{
    a = in64(a);
    uint32_t r = cvt_f32_f64(a);
    double v = fabs_d(f64(a));
    return out32(r, tiny32(r, v < 1.1754943508222875e-38 ? -1 : 0));
}
