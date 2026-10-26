/*
 * rt.h: the cortex-m7 state and exact instruction helpers the translated code runs on.
 * every helper implements the armv7-m / fpv5 pseudocode for the case the elf uses
 * (fpscr: rmode = nearest, fz = 0, dn = 0, ahp = 0: there is no vmsr in the elf).
 * compile with -fno-fast-math -ffp-contract=off: c float ops must be plain ieee.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_RT_H
#define LXR_RT_H
#include <stdint.h>
#include <string.h>

#define MEMSZ (4u << 20)            /* mps2-an500 ssram at 0: the whole elf */
#define PBASE 0x60000000u
#define PSZ (16u << 20)             /* mps2-an500 psram: qemu's heapinfo puts the stack here */
extern uint8_t M[MEMSZ + 16];
extern uint8_t P[PSZ + 16];
extern uint32_t R[16];
extern union fpregs { uint32_t s[32]; uint64_t d[16]; } F;
extern uint32_t FPSCR, PRIMASK, BASEPRI, CONTROL;
/* apsr flags. each translated function keeps them in locals (so dead flag writes vanish)
 * and syncs with these around every call and return: flags are architectural state, and
 * hand written asm (newlib, libgcc) may pass them across calls even if gcc's code never does. */
extern uint32_t gN, gZ, gC, gV, gGE;
#define FL_OUT() (gN = N, gZ = Z, gC = C, gV = V, gGE = GE)
#define FL_IN() (N = gN, Z = gZ, C = gC, V = gV, GE = gGE)

void rt_fatal(const char *what, uint32_t a);
uint32_t rt_sys_rd(uint32_t a, int n);
void rt_sys_wr(uint32_t a, uint32_t v, int n);
uint32_t rt_semihost(uint32_t op, uint32_t arg);
void rt_call(uint32_t addr);            /* indirect call, generated */

/* tracing (-DRT_TRACE): ring of function entries, dumped on RECOMP_TIMEOUT or a fatal */
#ifdef RT_TRACE
extern uint32_t rt_ring[4096]; extern uint32_t rt_ring_n;
#define RT_ENTER(a) (rt_ring[rt_ring_n++ & 4095u] = (a))
#else
#define RT_ENTER(a) ((void)0)
#endif

/* ------------------------------------------------------------------ memory */
/* host pointer for an n byte access at a, or 0 for the system space / unmapped */
static inline uint8_t *hp(uint32_t a, uint32_t n)
{
    if (a <= MEMSZ - n) return M + a;
    if (a - PBASE <= PSZ - n) return P + (a - PBASE);
    return 0;
}
static inline uint32_t ld32(uint32_t a) { uint32_t v; uint8_t *p = hp(a, 4); if (p) { memcpy(&v, p, 4); return v; } return rt_sys_rd(a, 4); }
static inline uint32_t ld16(uint32_t a) { uint16_t v; uint8_t *p = hp(a, 2); if (p) { memcpy(&v, p, 2); return v; } return rt_sys_rd(a, 2) & 0xffffu; }
static inline uint32_t ld8(uint32_t a) { uint8_t *p = hp(a, 1); if (p) return *p; return rt_sys_rd(a, 1) & 0xffu; }
static inline uint64_t ld64(uint32_t a) { return (uint64_t)ld32(a) | ((uint64_t)ld32(a + 4) << 32); }
static inline void st32(uint32_t a, uint32_t v) { uint8_t *p = hp(a, 4); if (p) memcpy(p, &v, 4); else rt_sys_wr(a, v, 4); }
static inline void st16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; uint8_t *p = hp(a, 2); if (p) memcpy(p, &x, 2); else rt_sys_wr(a, v & 0xffffu, 2); }
static inline void st8(uint32_t a, uint32_t v) { uint8_t *p = hp(a, 1); if (p) *p = (uint8_t)v; else rt_sys_wr(a, v & 0xffu, 1); }
static inline void st64(uint32_t a, uint64_t v) { st32(a, (uint32_t)v); st32(a + 4, (uint32_t)(v >> 32)); }

/* --------------------------------------------------------------- integer */
static inline uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
/* AddWithCarry: result, and carry / overflow out */
static inline uint32_t awc(uint32_t x, uint32_t y, uint32_t cin, uint32_t *c, uint32_t *v)
{
    uint64_t u = (uint64_t)x + y + cin;
    int64_t s = (int64_t)(int32_t)x + (int32_t)y + (int64_t)cin;
    uint32_t r = (uint32_t)u;
    *c = (u >> 32) & 1u;
    *v = (int64_t)(int32_t)r != s;
    return r;
}
/* shifts by register amount (bottom byte), with carry out; c = carry in */
static inline uint32_t lsl_c(uint32_t x, uint32_t n, uint32_t *c)
{ n &= 0xff; if (n == 0) return x; if (n < 32) { *c = (x >> (32 - n)) & 1u; return x << n; } *c = n == 32 ? (x & 1u) : 0; return 0; }
static inline uint32_t lsr_c(uint32_t x, uint32_t n, uint32_t *c)
{ n &= 0xff; if (n == 0) return x; if (n < 32) { *c = (x >> (n - 1)) & 1u; return x >> n; } *c = n == 32 ? (x >> 31) : 0; return 0; }
static inline uint32_t asr_c(uint32_t x, uint32_t n, uint32_t *c)
{ n &= 0xff; if (n == 0) return x; if (n < 32) { *c = (uint32_t)((int32_t)x >> (n - 1)) & 1u; return (uint32_t)((int32_t)x >> n); } *c = x >> 31; return (uint32_t)((int32_t)x >> 31); }
static inline uint32_t ror_c(uint32_t x, uint32_t n, uint32_t *c)
{ n &= 0xff; if (n == 0) return x; n &= 31; uint32_t r = n ? (x >> n) | (x << (32 - n)) : x; *c = r >> 31; return r; }
static inline uint32_t ssat(int32_t x, int bits)
{ int32_t hi = (int32_t)((1u << (bits - 1)) - 1), lo = -hi - 1; return (uint32_t)(x > hi ? hi : x < lo ? lo : x); }
static inline uint32_t usat(int32_t x, int bits)
{ int32_t hi = (int32_t)((1u << bits) - 1); return (uint32_t)(x > hi ? hi : x < 0 ? 0 : x); }
static inline uint32_t qadd16(uint32_t a, uint32_t b)
{ return (ssat((int16_t)a + (int16_t)b, 16) & 0xffffu) | (ssat((int16_t)(a >> 16) + (int16_t)(b >> 16), 16) << 16); }
static inline uint32_t qsub16(uint32_t a, uint32_t b)
{ return (ssat((int16_t)a - (int16_t)b, 16) & 0xffffu) | (ssat((int16_t)(a >> 16) - (int16_t)(b >> 16), 16) << 16); }
static inline uint32_t udiv(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t sdiv(uint32_t a, uint32_t b)
{ if (!b) return 0; if (a == 0x80000000u && b == 0xffffffffu) return a; return (uint32_t)((int32_t)a / (int32_t)b); }
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }

/* -------------------------------------------------------------- float32 */
static inline float f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t b32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline double f64(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
static inline uint64_t b64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

static inline double fabs_d(double x) { uint64_t b; memcpy(&b, &x, 8); b &= 0x7fffffffffffffffull; memcpy(&x, &b, 8); return x; }
static inline int isnan32(uint32_t b) { return (b & 0x7fffffffu) > 0x7f800000u; }
static inline int issnan32(uint32_t b) { return isnan32(b) && !(b & 0x00400000u); }
static inline int isnan64(uint64_t b) { return (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
static inline int issnan64(uint64_t b) { return isnan64(b) && !(b & 0x0008000000000000ull); }
#define DEFNAN32 0x7fc00000u
#define DEFNAN64 0x7ff8000000000000ull

/* FPProcessNaNs (dn = 0): first signalling nan quieted, else first quiet nan, else none */
static inline int nans2_32(uint32_t a, uint32_t b, uint32_t *r)
{
    if (issnan32(a)) { *r = a | 0x00400000u; return 1; }
    if (issnan32(b)) { *r = b | 0x00400000u; return 1; }
    if (isnan32(a)) { *r = a; return 1; }
    if (isnan32(b)) { *r = b; return 1; }
    return 0;
}
static inline int nans2_64(uint64_t a, uint64_t b, uint64_t *r)
{
    if (issnan64(a)) { *r = a | 0x0008000000000000ull; return 1; }
    if (issnan64(b)) { *r = b | 0x0008000000000000ull; return 1; }
    if (isnan64(a)) { *r = a; return 1; }
    if (isnan64(b)) { *r = b; return 1; }
    return 0;
}
static inline int nans3_32(uint32_t a, uint32_t b, uint32_t c, uint32_t *r)
{
    if (issnan32(a)) { *r = a | 0x00400000u; return 1; }
    if (issnan32(b)) { *r = b | 0x00400000u; return 1; }
    if (issnan32(c)) { *r = c | 0x00400000u; return 1; }
    if (isnan32(a)) { *r = a; return 1; }
    if (isnan32(b)) { *r = b; return 1; }
    if (isnan32(c)) { *r = c; return 1; }
    return 0;
}
static inline int nans3_64(uint64_t a, uint64_t b, uint64_t c, uint64_t *r)
{
    if (issnan64(a)) { *r = a | 0x0008000000000000ull; return 1; }
    if (issnan64(b)) { *r = b | 0x0008000000000000ull; return 1; }
    if (issnan64(c)) { *r = c | 0x0008000000000000ull; return 1; }
    if (isnan64(a)) { *r = a; return 1; }
    if (isnan64(b)) { *r = b; return 1; }
    if (isnan64(c)) { *r = c; return 1; }
    return 0;
}
/* a nan result from a plain ieee op with no nan input is the default nan */
#define F32OP(name, expr) static inline uint32_t name(uint32_t a, uint32_t b) \
    { uint32_t r; if (nans2_32(a, b, &r)) return r; r = b32(expr); return isnan32(r) ? DEFNAN32 : r; }
#define F64OP(name, expr) static inline uint64_t name(uint64_t a, uint64_t b) \
    { uint64_t r; if (nans2_64(a, b, &r)) return r; r = b64(expr); return isnan64(r) ? DEFNAN64 : r; }
F32OP(fadd32, f32(a) + f32(b))
F32OP(fsub32, f32(a) - f32(b))
F32OP(fmul32, f32(a) * f32(b))
F32OP(fdiv32, f32(a) / f32(b))
F64OP(fadd64, f64(a) + f64(b))
F64OP(fsub64, f64(a) - f64(b))
F64OP(fmul64, f64(a) * f64(b))
F64OP(fdiv64, f64(a) / f64(b))
static inline uint32_t fsqrt32(uint32_t a)
{ if (isnan32(a)) return issnan32(a) ? a | 0x00400000u : a; uint32_t r = b32(__builtin_sqrtf(f32(a))); return isnan32(r) ? DEFNAN32 : r; }
static inline uint32_t fneg32(uint32_t a) { return a ^ 0x80000000u; }
static inline uint32_t fabs32(uint32_t a) { return a & 0x7fffffffu; }
static inline uint64_t fneg64(uint64_t a) { return a ^ 0x8000000000000000ull; }
static inline uint64_t fabs64(uint64_t a) { return a & 0x7fffffffffffffffull; }

/* fused multiply add, one rounding. f32: the product is exact in f64; the f64 sum is made
 * round-to-odd (sticky bit from the exact error), then rounding to f32 is correct
 * (boldo & melquiond: round to odd at p+2 bits, then nearest at p). */
uint32_t fma32_exact(uint32_t a, uint32_t b, uint32_t c);   /* a*b + c */
uint64_t fma64_exact(uint64_t a, uint64_t b, uint64_t c);   /* a*b + c */
uint64_t fma64_exact_t(uint64_t a, uint64_t b, uint64_t c, int *tiny);
/* FPMulAdd(addend, op1, op2): nan order addend, op1, op2; inf*0 (+ anything) = default nan */
static inline uint32_t fmuladd32(uint32_t addend, uint32_t x, uint32_t y)
{
    uint32_t r;
    int inf0 = ((x & 0x7fffffffu) == 0x7f800000u && (y & 0x7fffffffu) == 0) || ((y & 0x7fffffffu) == 0x7f800000u && (x & 0x7fffffffu) == 0);
    if (nans3_32(addend, x, y, &r)) { if (inf0 && !issnan32(addend) && isnan32(addend)) return DEFNAN32; return r; }
    r = fma32_exact(x, y, addend);
    return isnan32(r) ? DEFNAN32 : r;
}
static inline uint64_t fmuladd64_t(uint64_t addend, uint64_t x, uint64_t y, int *tiny)
{
    uint64_t r;
    *tiny = 0;
    int inf0 = ((x & 0x7fffffffffffffffull) == 0x7ff0000000000000ull && (y & 0x7fffffffffffffffull) == 0) || ((y & 0x7fffffffffffffffull) == 0x7ff0000000000000ull && (x & 0x7fffffffffffffffull) == 0);
    if (nans3_64(addend, x, y, &r)) { if (inf0 && !issnan64(addend) && isnan64(addend)) return DEFNAN64; return r; }
    r = fma64_exact_t(x, y, addend, tiny);
    return isnan64(r) ? DEFNAN64 : r;
}
static inline uint64_t fmuladd64(uint64_t addend, uint64_t x, uint64_t y)
{
    uint64_t r;
    int inf0 = ((x & 0x7fffffffffffffffull) == 0x7ff0000000000000ull && (y & 0x7fffffffffffffffull) == 0) || ((y & 0x7fffffffffffffffull) == 0x7ff0000000000000ull && (x & 0x7fffffffffffffffull) == 0);
    if (nans3_64(addend, x, y, &r)) { if (inf0 && !issnan64(addend) && isnan64(addend)) return DEFNAN64; return r; }
    r = fma64_exact(x, y, addend);
    return isnan64(r) ? DEFNAN64 : r;
}

/* FPMaxNum / FPMinNum (vmaxnm, vminnm): a single quiet nan loses to a number (it is
 * treated as -inf for max, +inf for min), then FPMax / FPMin: nans processed, zeros by
 * sign (max: +0 unless both -0; min: -0 unless both +0), else the larger / smaller. */
#define FMINMAX(name, T, FT, CV, ISNAN, ISSNAN, NANS2, INFNEG, INFPOS, SIGN, MAX) \
static inline T name(T a, T b) \
{ \
    int qa = ISNAN(a) && !ISSNAN(a), qb = ISNAN(b) && !ISSNAN(b); \
    if (qa && !qb) a = MAX ? INFNEG : INFPOS; else if (!qa && qb) b = MAX ? INFNEG : INFPOS; \
    T r; if (NANS2(a, b, &r)) return r; \
    if (!((a | b) & ~SIGN)) return MAX ? (a & b) : (a | b); \
    FT x = CV(a), y = CV(b); \
    return MAX ? (x > y ? a : b) : (x < y ? a : b); \
}
FMINMAX(fmaxnm32, uint32_t, float, f32, isnan32, issnan32, nans2_32, 0xff800000u, 0x7f800000u, 0x80000000u, 1)
FMINMAX(fminnm32, uint32_t, float, f32, isnan32, issnan32, nans2_32, 0xff800000u, 0x7f800000u, 0x80000000u, 0)
FMINMAX(fmaxnm64, uint64_t, double, f64, isnan64, issnan64, nans2_64, 0xfff0000000000000ull, 0x7ff0000000000000ull, 0x8000000000000000ull, 1)
FMINMAX(fminnm64, uint64_t, double, f64, isnan64, issnan64, nans2_64, 0xfff0000000000000ull, 0x7ff0000000000000ull, 0x8000000000000000ull, 0)
/* vrintz: round toward zero to an integral value (sign of zero kept); nan processed */
static inline uint32_t frintz32(uint32_t a) { if (isnan32(a)) return a | 0x00400000u; return b32(__builtin_truncf(f32(a))); }
static inline uint64_t frintz64(uint64_t a) { if (isnan64(a)) return a | 0x0008000000000000ull; return b64(__builtin_trunc(f64(a))); }

/* vcmp: fpscr nzcv. unordered 0011, equal 0110, less 1000, greater 0010 */
static inline uint32_t fcmp32(uint32_t a, uint32_t b)
{ if (isnan32(a) || isnan32(b)) return 0x3u; float x = f32(a), y = f32(b); return x == y ? 0x6u : x < y ? 0x8u : 0x2u; }
static inline uint32_t fcmp64(uint64_t a, uint64_t b)
{ if (isnan64(a) || isnan64(b)) return 0x3u; double x = f64(a), y = f64(b); return x == y ? 0x6u : x < y ? 0x8u : 0x2u; }

/* conversions. float -> int: round toward zero, saturate, nan -> 0 */
static inline uint32_t cvt_s32_f64d(double x)
{ if (x != x) return 0; if (x >= 2147483648.0) return 0x7fffffffu; if (x <= -2147483649.0) return 0x80000000u; return (uint32_t)(int32_t)x; }
static inline uint32_t cvt_u32_f64d(double x)
{ if (x != x) return 0; if (x >= 4294967296.0) return 0xffffffffu; if (x <= -1.0) return 0; return (uint32_t)x; }
static inline uint32_t cvt_s32_f32(uint32_t a) { return cvt_s32_f64d((double)f32(a)); }
static inline uint32_t cvt_u32_f32(uint32_t a) { return cvt_u32_f64d((double)f32(a)); }
static inline uint32_t cvt_s32_f64(uint64_t a) { return cvt_s32_f64d(f64(a)); }
static inline uint32_t cvt_s32_f32_fix(uint32_t a, int fb) { return cvt_s32_f64d((double)f32(a) * (double)(1ull << fb)); }
static inline uint32_t cvt_f32_s32(uint32_t a) { return b32((float)(int32_t)a); }
static inline uint32_t cvt_f32_u32(uint32_t a) { return b32((float)a); }
static inline uint32_t cvt_f32_s32_fix(uint32_t a, int fb) { return b32((float)((double)(int32_t)a / (double)(1ull << fb))); }
static inline uint64_t cvt_f64_s32(uint32_t a) { return b64((double)(int32_t)a); }
/* f32 <-> f64: nans keep sign and top payload bits, quieted */
static inline uint64_t cvt_f64_f32(uint32_t a)
{ if (isnan32(a)) return ((uint64_t)(a >> 31) << 63) | 0x7ff8000000000000ull | ((uint64_t)(a & 0x003fffffu) << 29); return b64((double)f32(a)); }
static inline uint32_t cvt_f32_f64(uint64_t a)
{ if (isnan64(a)) return ((uint32_t)(a >> 63) << 31) | 0x7fc00000u | (uint32_t)((a >> 29) & 0x003fffffu); return b32((float)f64(a)); }
uint32_t cvt_f32_f16(uint32_t h);   /* bottom 16 bits in */
uint32_t cvt_f16_f32(uint32_t a);   /* 16 bits out, nearest even, ahp = 0 */

/* ------------------------------------------------- FPSCR.FZ dispatch (fz.c) */
#define FZ_ON (__builtin_expect((FPSCR >> 24) & 1u, 0))
void rt_fz_mode_check(void);
uint32_t fz_add32(uint32_t, uint32_t, int); uint32_t fz_mul32(uint32_t, uint32_t); uint32_t fz_div32(uint32_t, uint32_t);
uint32_t fz_sqrt32(uint32_t); uint32_t fz_fmuladd32(uint32_t, uint32_t, uint32_t); uint32_t fz_cmp32(uint32_t, uint32_t);
uint32_t fz_maxnm32(uint32_t, uint32_t); uint32_t fz_minnm32(uint32_t, uint32_t); uint32_t fz_rintz32(uint32_t);
uint32_t fz_cvt_s32_f32(uint32_t); uint32_t fz_cvt_u32_f32(uint32_t); uint32_t fz_cvt_s32_f32_fix(uint32_t, int);
uint64_t fz_cvt_f64_f32(uint32_t); uint32_t fz_cvt_f16_f32(uint32_t);
uint64_t fz_add64(uint64_t, uint64_t, int); uint64_t fz_mul64(uint64_t, uint64_t); uint64_t fz_div64(uint64_t, uint64_t);
uint64_t fz_fmuladd64(uint64_t, uint64_t, uint64_t); uint32_t fz_cmp64(uint64_t, uint64_t);
uint64_t fz_maxnm64(uint64_t, uint64_t); uint64_t fz_minnm64(uint64_t, uint64_t); uint64_t fz_rintz64(uint64_t);
uint32_t fz_cvt_s32_f64(uint64_t); uint32_t fz_cvt_f32_f64(uint64_t);
static inline uint32_t xadd32(uint32_t a, uint32_t b) { return FZ_ON ? fz_add32(a, b, 0) : fadd32(a, b); }
static inline uint32_t xsub32(uint32_t a, uint32_t b) { return FZ_ON ? fz_add32(a, b, 1) : fsub32(a, b); }
static inline uint32_t xmul32(uint32_t a, uint32_t b) { return FZ_ON ? fz_mul32(a, b) : fmul32(a, b); }
static inline uint32_t xdiv32(uint32_t a, uint32_t b) { return FZ_ON ? fz_div32(a, b) : fdiv32(a, b); }
static inline uint32_t xsqrt32(uint32_t a) { return FZ_ON ? fz_sqrt32(a) : fsqrt32(a); }
static inline uint32_t xmuladd32(uint32_t d, uint32_t x, uint32_t y) { return FZ_ON ? fz_fmuladd32(d, x, y) : fmuladd32(d, x, y); }
static inline uint32_t xcmp32(uint32_t a, uint32_t b) { return FZ_ON ? fz_cmp32(a, b) : fcmp32(a, b); }
static inline uint32_t xmaxnm32(uint32_t a, uint32_t b) { return FZ_ON ? fz_maxnm32(a, b) : fmaxnm32(a, b); }
static inline uint32_t xminnm32(uint32_t a, uint32_t b) { return FZ_ON ? fz_minnm32(a, b) : fminnm32(a, b); }
static inline uint32_t xrintz32(uint32_t a) { return FZ_ON ? fz_rintz32(a) : frintz32(a); }
static inline uint32_t xcvt_s32_f32(uint32_t a) { return FZ_ON ? fz_cvt_s32_f32(a) : cvt_s32_f32(a); }
static inline uint32_t xcvt_u32_f32(uint32_t a) { return FZ_ON ? fz_cvt_u32_f32(a) : cvt_u32_f32(a); }
static inline uint32_t xcvt_s32_f32_fix(uint32_t a, int fb) { return FZ_ON ? fz_cvt_s32_f32_fix(a, fb) : cvt_s32_f32_fix(a, fb); }
static inline uint64_t xcvt_f64_f32(uint32_t a) { return FZ_ON ? fz_cvt_f64_f32(a) : cvt_f64_f32(a); }
static inline uint32_t xcvt_f16_f32(uint32_t a) { return FZ_ON ? fz_cvt_f16_f32(a) : cvt_f16_f32(a); }
static inline uint64_t xadd64(uint64_t a, uint64_t b) { return FZ_ON ? fz_add64(a, b, 0) : fadd64(a, b); }
static inline uint64_t xsub64(uint64_t a, uint64_t b) { return FZ_ON ? fz_add64(a, b, 1) : fsub64(a, b); }
static inline uint64_t xmul64(uint64_t a, uint64_t b) { return FZ_ON ? fz_mul64(a, b) : fmul64(a, b); }
static inline uint64_t xdiv64(uint64_t a, uint64_t b) { return FZ_ON ? fz_div64(a, b) : fdiv64(a, b); }
static inline uint64_t xmuladd64(uint64_t d, uint64_t x, uint64_t y) { return FZ_ON ? fz_fmuladd64(d, x, y) : fmuladd64(d, x, y); }
static inline uint32_t xcmp64(uint64_t a, uint64_t b) { return FZ_ON ? fz_cmp64(a, b) : fcmp64(a, b); }
static inline uint64_t xmaxnm64(uint64_t a, uint64_t b) { return FZ_ON ? fz_maxnm64(a, b) : fmaxnm64(a, b); }
static inline uint64_t xminnm64(uint64_t a, uint64_t b) { return FZ_ON ? fz_minnm64(a, b) : fminnm64(a, b); }
static inline uint64_t xrintz64(uint64_t a) { return FZ_ON ? fz_rintz64(a) : frintz64(a); }
static inline uint32_t xcvt_s32_f64(uint64_t a) { return FZ_ON ? fz_cvt_s32_f64(a) : cvt_s32_f64(a); }
static inline uint32_t xcvt_f32_f64(uint64_t a) { return FZ_ON ? fz_cvt_f32_f64(a) : cvt_f32_f64(a); }

#endif
