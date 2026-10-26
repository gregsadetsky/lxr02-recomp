/*
 * lxr_intrinsics.h: portable c versions of the cmsis core intrinsics, wasm only.
 * the dsp ones (ssat/usat/qadd16/qsub16/sadd16/qadd/qsub/pkhbt/smlald/clz) are the
 * versions verified bit-exact against the cortex-m7 instructions in
 * 1.dsp-wasm-poc (40000 edge vectors raw + 2e6 random hashed).
 * system ones model state the virtual board reads (primask) or are no-ops
 * (barriers, wfi). SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_INTRINSICS_H
#define LXR_INTRINSICS_H
#include <stdint.h>

/* ---- dsp intrinsics, verified ---- */
/* ssat: saturate signed 32-bit value to a signed sat-bit range */
static inline int32_t lxr_ssat(int32_t val, uint32_t sat)
{
    if (sat >= 1u && sat <= 32u) {
        const int32_t max = (int32_t)((1u << (sat - 1u)) - 1u);
        const int32_t min = -1 - max;
        if (val > max) return max;
        if (val < min) return min;
    }
    return val;
}
/* usat: saturate signed 32-bit value to an unsigned sat-bit range */
static inline uint32_t lxr_usat(int32_t val, uint32_t sat)
{
    if (sat <= 31u) {
        const uint32_t max = (1u << sat) - 1u;
        if (val > (int32_t)max) return max;
        if (val < 0) return 0u;
    } else if (val < 0) {
        return 0u;
    }
    return (uint32_t)val;
}
#define __SSAT(ARG1, ARG2) lxr_ssat((int32_t)(ARG1), (ARG2))
#define __USAT(ARG1, ARG2) lxr_usat((int32_t)(ARG1), (ARG2))

static inline int32_t lxr_sat16(int32_t x)
{
    return x > 32767 ? 32767 : (x < -32768 ? -32768 : x);
}
/* qadd16 / qsub16: per-halfword signed saturating add / sub */
static inline uint32_t __QADD16(uint32_t op1, uint32_t op2)
{
    const int32_t lo = lxr_sat16((int32_t)(int16_t)(op1 & 0xFFFFu) + (int32_t)(int16_t)(op2 & 0xFFFFu));
    const int32_t hi = lxr_sat16((int32_t)(int16_t)(op1 >> 16) + (int32_t)(int16_t)(op2 >> 16));
    return ((uint32_t)hi << 16) | ((uint32_t)lo & 0xFFFFu);
}
static inline uint32_t __QSUB16(uint32_t op1, uint32_t op2)
{
    const int32_t lo = lxr_sat16((int32_t)(int16_t)(op1 & 0xFFFFu) - (int32_t)(int16_t)(op2 & 0xFFFFu));
    const int32_t hi = lxr_sat16((int32_t)(int16_t)(op1 >> 16) - (int32_t)(int16_t)(op2 >> 16));
    return ((uint32_t)hi << 16) | ((uint32_t)lo & 0xFFFFu);
}
/* sadd16: per-halfword wrapping add (ge flags not modelled) */
static inline uint32_t __SADD16(uint32_t op1, uint32_t op2)
{
    const uint32_t lo = (uint32_t)((int32_t)(int16_t)(op1 & 0xFFFFu) + (int32_t)(int16_t)(op2 & 0xFFFFu));
    const uint32_t hi = (uint32_t)((int32_t)(int16_t)(op1 >> 16) + (int32_t)(int16_t)(op2 >> 16));
    return (hi << 16) | (lo & 0xFFFFu);
}
/* qadd / qsub: 32-bit signed saturating add / sub */
static inline int32_t __QADD(int32_t op1, int32_t op2)
{
    const int64_t r = (int64_t)op1 + (int64_t)op2;
    return r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
}
static inline int32_t __QSUB(int32_t op1, int32_t op2)
{
    const int64_t r = (int64_t)op1 - (int64_t)op2;
    return r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
}
/* pkhbt: low halfword of arg1, high halfword of (arg2 << arg3) */
#define __PKHBT(ARG1, ARG2, ARG3) \
    ((((uint32_t)(ARG1)) & 0x0000FFFFUL) | ((((uint32_t)(ARG2)) << (ARG3)) & 0xFFFF0000UL))
/* smlald: dual signed 16x16 multiply, 64-bit accumulate */
static inline uint64_t __SMLALD(uint32_t op1, uint32_t op2, uint64_t acc)
{
    /* products fit in int32; the accumulate wraps modulo 2^64 like the
     * instruction, so it is done in unsigned arithmetic (no signed overflow ub) */
    const int32_t p0 = (int32_t)(int16_t)(op1 & 0xFFFFu) * (int32_t)(int16_t)(op2 & 0xFFFFu);
    const int32_t p1 = (int32_t)(int16_t)(op1 >> 16) * (int32_t)(int16_t)(op2 >> 16);
    return acc + (uint64_t)(int64_t)p0 + (uint64_t)(int64_t)p1;
}
/* clz: count leading zeros, clz(0) == 32 on arm */
static inline uint8_t __CLZ(uint32_t value)
{
    return value == 0u ? 32u : (uint8_t)__builtin_clz(value);
}


/* ---- system intrinsics ---- */
extern volatile uint32_t lxr_primask;    /* 1 = interrupts masked; the board's dispatcher honours it */
extern volatile uint32_t lxr_fpscr;
static inline void __enable_irq(void)  { lxr_primask = 0u; }
static inline void __disable_irq(void) { lxr_primask = 1u; }
static inline uint32_t __get_PRIMASK(void) { return lxr_primask; }
static inline void __set_PRIMASK(uint32_t v) { lxr_primask = v & 1u; }
static inline uint32_t __get_FPSCR(void) { return lxr_fpscr; }
static inline void __set_FPSCR(uint32_t v) { lxr_fpscr = v; }
static inline uint32_t __get_CONTROL(void) { return 0u; }
static inline void __set_CONTROL(uint32_t v) { (void)v; }
static inline uint32_t __get_IPSR(void) { return 0u; }
static inline uint32_t __get_xPSR(void) { return 0u; }
static inline uint32_t __get_MSP(void) { return 0u; }
static inline void __set_MSP(uint32_t v) { (void)v; }
static inline uint32_t __get_PSP(void) { return 0u; }
static inline void __set_PSP(uint32_t v) { (void)v; }
static inline uint32_t __get_BASEPRI(void) { return 0u; }
static inline void __set_BASEPRI(uint32_t v) { (void)v; }
static inline uint32_t __get_FAULTMASK(void) { return 0u; }
static inline void __set_FAULTMASK(uint32_t v) { (void)v; }
static inline void __enable_fault_irq(void) {}
static inline void __disable_fault_irq(void) {}
/* apsr: only read by Oscillator.c::calcNoise, which is dead code (its callers are commented out) */
static inline uint32_t __get_APSR(void) { return 0u; }
#define __NOP()   ((void)0)
#define __WFI()   ((void)0)
#define __WFE()   ((void)0)
#define __SEV()   ((void)0)
#define __BKPT(v) ((void)(v))
static inline void __ISB(void) {}
static inline void __DSB(void) {}
static inline void __DMB(void) {}
static inline uint32_t __REV(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t __REV16(uint32_t v) { return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8); }
static inline int16_t __REVSH(int16_t v) { return (int16_t)__builtin_bswap16((uint16_t)v); }
static inline uint32_t __ROR(uint32_t v, uint32_t s) { s &= 31u; return s ? (v >> s) | (v << (32u - s)) : v; }
static inline uint32_t __RBIT(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1u); v >>= 1; } return r; }
#endif
