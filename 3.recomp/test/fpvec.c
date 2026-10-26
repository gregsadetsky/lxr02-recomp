/*
 * fpvec.c: every vfp / dsp instruction form the firmware elf uses, run through inline asm
 * on edge and random operands. built for cortex-m7, run on qemu (the reference) and through
 * the recompiler; the two output files must be identical. writes fpvec.bin.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t __StackTop;
extern void _start(void);
void tv_reset(void)
{
    *(volatile uint32_t *)0xe000ed88u |= 0xfu << 20;   /* cpacr: fpu on */
    __asm volatile ("dsb\n isb");
    _start();
    for (;;) {}
}
__attribute__((section(".isr_vector"), used)) const void *const tv_vectors[2] = { &__StackTop, tv_reset };

static FILE *out;
static uint32_t buf[4096]; static unsigned nb;
static void tvput(uint32_t v) { buf[nb++] = v; if (nb == 4096) { fwrite(buf, 4, nb, out); nb = 0; } }
/* records: 0xa5<line> v (32 bit result), 0xa6<line> lo hi (64 bit result), 0xb<n><line> + n input words */
#define put(v) do { tvput(0xa5000000u | __LINE__); tvput(v); } while (0)
#define put64(v) do { uint64_t w_ = (v); tvput(0xa6000000u | __LINE__); tvput((uint32_t)w_); tvput((uint32_t)(w_ >> 32)); } while (0)
#define IN(n, ...) do { uint32_t in_[] = { __VA_ARGS__ }; tvput(0xb0000000u | ((n) << 24) | __LINE__); for (int k_ = 0; k_ < (n); k_++) tvput(in_[k_]); } while (0)

static uint32_t rs = 0x12345678u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
/* random float bits, biased to interesting exponents (near 0, near 1, huge, denormal) */
static uint32_t rf32(void)
{
    uint32_t r = rnd(), s = r & 0x80000000u, k = rnd() & 7, m = rnd() & 0x7fffffu, e;
    switch (k) {
    case 0: e = 0; break;
    case 1: e = 0xff; break;
    case 2: e = 1 + (rnd() & 3); break;
    case 3: e = 0xfe - (rnd() & 3); break;
    case 4: case 5: e = 120 + (rnd() & 15); break;
    default: e = rnd() & 0xff;
    }
    if (k == 6) m &= 0x7ff000u;                         /* short mantissas: exact products, ties */
    return s | (e << 23) | m;
}
static uint64_t rf64(void)
{
    uint64_t s = (uint64_t)(rnd() & 1u) << 63, m = ((uint64_t)rnd() << 32 | rnd()) & 0xfffffffffffffull, e;
    uint32_t k = rnd() & 7;
    switch (k) {
    case 0: e = 0; break;
    case 1: e = 0x7ff; break;
    case 2: e = 1 + (rnd() & 3); break;
    case 3: e = 0x7fe - (rnd() & 3); break;
    case 4: case 5: e = 1015 + (rnd() & 15); break;
    default: e = rnd() & 0x7ff;
    }
    if (k == 6) m &= 0xfffffc0000000ull;
    return s | (e << 52) | m;
}

static const uint32_t E32[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
    0x3f800000, 0xbf800000, 0x3f800001, 0x3f7fffff, 0x40000000, 0x3f000000, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x7fc12345,
    0x7f812345, 0x4b000000, 0x4b800000, 0x4f000000, 0xcf000000, 0x4f800000, 0x4effffff, 0xcf000001,
    0x3eaaaaab, 0x40490fdb, 0x3dcccccd, 0x33800000, 0x34000000, 0x38800000, 0x387fc000, 0x477fe000,
    0x477ff000, 0x47800000, 0xc77fe000, 0x33000000, 0x33000001, 0x337fffff, 0x3f800000 | 0x1000,
};
static const uint64_t E64[] = {
    0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull, 0x0010000000000000ull,
    0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3ff0000000000001ull, 0x3fefffffffffffffull,
    0x4000000000000000ull, 0x3fe0000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull,
    0x7ff0000000000001ull, 0x7ff4000000000000ull, 0x7ff8000000012345ull, 0x41e0000000000000ull,
    0xc1e0000000000000ull, 0x41dfffffffc00000ull, 0xc1e0000000200000ull, 0x41efffffffe00000ull,
    0x3fd5555555555555ull, 0x400921fb54442d18ull, 0x3cb0000000000000ull, 0x3ca0000000000000ull,
    0x36a0000000000000ull, 0x380fffffe0000000ull, 0x47efffffe0000000ull, 0x47effffff0000000ull,
};
#define NE32 (sizeof E32 / sizeof E32[0])
#define NE64 (sizeof E64 / sizeof E64[0])

#define F32(x) ({ float f_; uint32_t u_ = (x); memcpy(&f_, &u_, 4); f_; })
#define B32(f) ({ uint32_t u_; float f_ = (f); memcpy(&u_, &f_, 4); u_; })
#define F64(x) ({ double f_; uint64_t u_ = (x); memcpy(&f_, &u_, 8); f_; })
#define B64(f) ({ uint64_t u_; double f_ = (f); memcpy(&u_, &f_, 8); u_; })

#define OP2S(ins) ({ float r_, a_ = F32(a), b_ = F32(b); __asm volatile (ins " %0, %1, %2" : "=t"(r_) : "t"(a_), "t"(b_)); B32(r_); })
#define OP2D(ins) ({ double r_, a_ = F64(a), b_ = F64(b); __asm volatile (ins " %P0, %P1, %P2" : "=w"(r_) : "w"(a_), "w"(b_)); B64(r_); })

static void bin32(uint32_t a, uint32_t b)
{
    IN(2, a, b);
    put(OP2S("vadd.f32")); put(OP2S("vsub.f32")); put(OP2S("vmul.f32")); put(OP2S("vdiv.f32"));
    uint32_t fl;
    { float a_ = F32(a), b_ = F32(b); __asm volatile ("vcmp.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(fl) : "t"(a_), "t"(b_) : "cc"); put(fl >> 28); }
    { float a_ = F32(a), b_ = F32(b); __asm volatile ("vcmpe.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(fl) : "t"(a_), "t"(b_) : "cc"); put(fl >> 28); }
    { float r_, a_ = F32(a), b_ = F32(b); __asm volatile ("vcmp.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n vselgt.f32 %0, %1, %2" : "=t"(r_) : "t"(a_), "t"(b_) : "cc"); put(B32(r_)); }
    put(OP2S("vmaxnm.f32")); put(OP2S("vminnm.f32"));
    { float r_, a_ = F32(a), b_ = F32(b); __asm volatile ("vcmp.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n vseleq.f32 %0, %1, %2" : "=t"(r_) : "t"(a_), "t"(b_) : "cc"); put(B32(r_)); }
}
static void un32(uint32_t a)
{
    IN(1, a);
    float a_ = F32(a), r_; uint32_t u;
    __asm volatile ("vsqrt.f32 %0, %1" : "=t"(r_) : "t"(a_)); put(B32(r_));
    __asm volatile ("vabs.f32 %0, %1" : "=t"(r_) : "t"(a_)); put(B32(r_));
    __asm volatile ("vrintz.f32 %0, %1" : "=t"(r_) : "t"(a_)); put(B32(r_));
    __asm volatile ("vneg.f32 %0, %1" : "=t"(r_) : "t"(a_)); put(B32(r_));
    __asm volatile ("vcvt.s32.f32 %0, %1" : "=t"(r_) : "t"(a_)); memcpy(&u, &r_, 4); put(u);
    __asm volatile ("vcvt.u32.f32 %0, %1" : "=t"(r_) : "t"(a_)); memcpy(&u, &r_, 4); put(u);
    r_ = a_; __asm volatile ("vcvt.s32.f32 %0, %0, #15" : "+t"(r_)); memcpy(&u, &r_, 4); put(u);
    r_ = a_; __asm volatile ("vcvt.f32.s32 %0, %0" : "+t"(r_)); put(B32(r_));     /* bits as int */
    r_ = a_; __asm volatile ("vcvt.f32.u32 %0, %0" : "+t"(r_)); put(B32(r_));
    r_ = a_; __asm volatile ("vcvt.f32.s32 %0, %0, #12" : "+t"(r_)); put(B32(r_));
    { double d_; __asm volatile ("vcvt.f64.f32 %P0, %1" : "=w"(d_) : "t"(a_)); put64(B64(d_)); }
    { double d_; __asm volatile ("vcvt.f64.s32 %P0, %1" : "=w"(d_) : "t"(a_)); put64(B64(d_)); }
    r_ = a_; __asm volatile ("vcvtb.f32.f16 %0, %0" : "+t"(r_)); put(B32(r_));
    r_ = F32(0xabcd0000u | (a & 0xffffu)); __asm volatile ("vcvtb.f32.f16 %0, %0" : "+t"(r_)); put(B32(r_));
    r_ = F32(0x12340000u); __asm volatile ("vcvtb.f16.f32 %0, %1" : "+t"(r_) : "t"(a_)); put(B32(r_));
    { __asm volatile ("vcmp.f32 %1, #0\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(u) : "t"(a_) : "cc"); put(u >> 28); }
    { __asm volatile ("vcmpe.f32 %1, #0\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(u) : "t"(a_) : "cc"); put(u >> 28); }
}
static void fma32(uint32_t a, uint32_t b, uint32_t c)
{
    IN(3, a, b, c);
    float d_ = F32(c), a_ = F32(a), b_ = F32(b);
    __asm volatile ("vfma.f32 %0, %1, %2" : "+t"(d_) : "t"(a_), "t"(b_)); put(B32(d_));
}
static void bin64(uint64_t a, uint64_t b)
{
    IN(4, (uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b, (uint32_t)(b >> 32));
    put64(OP2D("vadd.f64")); put64(OP2D("vsub.f64")); put64(OP2D("vmul.f64")); put64(OP2D("vdiv.f64"));
    put64(OP2D("vmaxnm.f64")); put64(OP2D("vminnm.f64"));
    uint32_t fl;
    { double a_ = F64(a), b_ = F64(b); __asm volatile ("vcmp.f64 %P1, %P2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(fl) : "w"(a_), "w"(b_) : "cc"); put(fl >> 28); }
    { double a_ = F64(a), b_ = F64(b); __asm volatile ("vcmpe.f64 %P1, %P2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(fl) : "w"(a_), "w"(b_) : "cc"); put(fl >> 28); }
    { double r_, a_ = F64(a), b_ = F64(b); __asm volatile ("vcmp.f64 %P1, %P2\n vmrs APSR_nzcv, fpscr\n vselgt.f64 %P0, %P1, %P2" : "=w"(r_) : "w"(a_), "w"(b_) : "cc"); put64(B64(r_)); }
}
static void un64(uint64_t a)
{
    IN(2, (uint32_t)a, (uint32_t)(a >> 32));
    double a_ = F64(a), r_; float s_; uint32_t u;
    __asm volatile ("vabs.f64 %P0, %P1" : "=w"(r_) : "w"(a_)); put64(B64(r_));
    __asm volatile ("vrintz.f64 %P0, %P1" : "=w"(r_) : "w"(a_)); put64(B64(r_));
    __asm volatile ("vneg.f64 %P0, %P1" : "=w"(r_) : "w"(a_)); put64(B64(r_));
    __asm volatile ("vcvt.f32.f64 %0, %P1" : "=t"(s_) : "w"(a_)); put(B32(s_));
    __asm volatile ("vcvt.s32.f64 %0, %P1" : "=t"(s_) : "w"(a_)); memcpy(&u, &s_, 4); put(u);
    { __asm volatile ("vcmp.f64 %P1, #0\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(u) : "w"(a_) : "cc"); put(u >> 28); }
    { __asm volatile ("vcmpe.f64 %P1, #0\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(u) : "w"(a_) : "cc"); put(u >> 28); }
}
static void fma64(uint64_t a, uint64_t b, uint64_t c)
{
    IN(6, (uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b, (uint32_t)(b >> 32), (uint32_t)c, (uint32_t)(c >> 32));
    double d_, a_ = F64(a), b_ = F64(b);
    d_ = F64(c); __asm volatile ("vfma.f64 %P0, %P1, %P2" : "+w"(d_) : "w"(a_), "w"(b_)); put64(B64(d_));
    d_ = F64(c); __asm volatile ("vfnms.f64 %P0, %P1, %P2" : "+w"(d_) : "w"(a_), "w"(b_)); put64(B64(d_));
    d_ = F64(c); __asm volatile ("vmla.f64 %P0, %P1, %P2" : "+w"(d_) : "w"(a_), "w"(b_)); put64(B64(d_));
    d_ = F64(c); __asm volatile ("vnmls.f64 %P0, %P1, %P2" : "+w"(d_) : "w"(a_), "w"(b_)); put64(B64(d_));
}
static void ints(uint32_t a, uint32_t b)
{
    IN(2, a, b);
    uint32_t r, fl;
    __asm volatile ("ssat %0, #16, %1" : "=r"(r) : "r"(a)); put(r);
    __asm volatile ("usat %0, #7, %1" : "=r"(r) : "r"(a)); put(r);
    __asm volatile ("qadd16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); put(r);
    __asm volatile ("qsub16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); put(r);
    __asm volatile ("udiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b & 0x8000000fu)); put(r);
    __asm volatile ("sdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"((b & 1) ? 0xffffffffu : (b & 0x8000000fu))); put(r);
    __asm volatile ("uadd8 %0, %1, %2\n sel %0, %1, %2" : "=&r"(r) : "r"(a), "r"(b)); put(r);
    __asm volatile ("clz %0, %1" : "=r"(r) : "r"(a >> (b & 31))); put(r);
    __asm volatile ("smulbb %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); put(r);
    __asm volatile ("lsls %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b & 0x3f) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("lsrs %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b & 0x3f) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("asrs %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b & 0x3f) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("adds %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("subs %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("cmp %2, %3\n sbcs %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("cmn %2, %3\n adcs %0, %2, %3\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("ands %0, %2, %3, lsr #9\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a), "r"(b) : "cc"); put(r); put(fl >> 28);
    __asm volatile ("ands %0, %2, #0xff000000\n mrs %1, apsr" : "=&r"(r), "=&r"(fl) : "r"(a) : "cc"); put(r); put(fl >> 28);
    { uint32_t lo, hi; __asm volatile ("umull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(a), "r"(b)); put(lo); put(hi); }
    { uint32_t lo, hi; __asm volatile ("smull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(a), "r"(b)); put(lo); put(hi); }
    { uint32_t lo = b, hi = a; __asm volatile ("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi); }
}

int main(void)
{
    out = fopen("fpvec.bin", "wb");
    if (!out) return 1;
    for (unsigned i = 0; i < NE32; i++) { un32(E32[i]); for (unsigned j = 0; j < NE32; j++) { bin32(E32[i], E32[j]); for (unsigned k = 0; k < NE32; k++) fma32(E32[i], E32[j], E32[k]); } }
    for (unsigned i = 0; i < NE64; i++) { un64(E64[i]); for (unsigned j = 0; j < NE64; j++) { bin64(E64[i], E64[j]); for (unsigned k = 0; k < NE64; k++) fma64(E64[i], E64[j], E64[k]); } }
    for (unsigned n = 0; n < 200000; n++) {
        uint32_t a = rf32(), b = rf32(), c = rf32();
        un32(a); bin32(a, b); fma32(a, b, c);
        /* near-cancellation and sticky probes for the fused paths */
        float p; { float a_ = F32(a), b_ = F32(b); __asm volatile ("vmul.f32 %0, %1, %2" : "=t"(p) : "t"(a_), "t"(b_)); }
        fma32(a, b, B32(p) ^ 0x80000000u); fma32(a, b, (B32(p) ^ 0x80000000u) + (rnd() & 7) - 3);
        uint64_t x = rf64(), y = rf64(), z = rf64();
        un64(x); bin64(x, y); fma64(x, y, z);
        double q; { double x_ = F64(x), y_ = F64(y); __asm volatile ("vmul.f64 %P0, %P1, %P2" : "=w"(q) : "w"(x_), "w"(y_)); }
        fma64(x, y, B64(q) ^ 0x8000000000000000ull); fma64(x, y, (B64(q) ^ 0x8000000000000000ull) + (rnd() & 7) - 3);
        ints(rnd(), rnd());
        if ((n & 1023) == 0) ints(E32[n % NE32], E32[(n / 7) % NE32]);
    }
    fwrite(buf, 4, nb, out);
    fclose(out);
    printf("fpvec done\n");
    return 0;
}
