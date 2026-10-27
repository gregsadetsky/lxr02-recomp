/*
 * rt.c: machine state, the cortex-m7 system space the elf touches, semihosting the way
 * qemu (mps2-an500, -semihosting-config target=native) answers it, and the loader.
 * every answer here was read off qemu for this elf (gdb stub, see 0.notes), not assumed.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

uint8_t M[MEMSZ + 16];
uint8_t P[PSZ + 16];
uint32_t R[16];
union fpregs F;
uint32_t FPSCR, PRIMASK, BASEPRI, CONTROL;
uint32_t gN, gZ, gC, gV, gGE;

extern const struct rt_seg { uint32_t addr, len; const uint8_t *data; } rt_segs[];
extern const unsigned rt_nsegs;
extern const uint32_t rt_entry;
void rt_call(uint32_t a);

#ifdef RT_TRACE
uint32_t rt_ring[4096]; uint32_t rt_ring_n;
#endif
static void dump_state(void)
{
    fprintf(stderr, "recomp: r0-r3 %08x %08x %08x %08x r4-r7 %08x %08x %08x %08x\n", R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7]);
    fprintf(stderr, "recomp: r8-r12 %08x %08x %08x %08x %08x sp %08x lr %08x\n", R[8], R[9], R[10], R[11], R[12], R[13], R[14]);
#ifdef RT_TRACE
    fprintf(stderr, "recomp: last function entries (oldest first):");
    for (uint32_t k = rt_ring_n > 48 ? rt_ring_n - 48 : 0; k < rt_ring_n; k++) fprintf(stderr, " %x", rt_ring[k & 4095u]);
    fprintf(stderr, "\n");
#endif
}
void rt_fatal(const char *what, uint32_t a)
{
    fprintf(stderr, "recomp: %s at 0x%08x\n", what, a);
    dump_state();
    exit(70);
}
#include <signal.h>
#include <unistd.h>
static void on_alarm(int sig) { (void)sig; fprintf(stderr, "recomp: timeout\n"); dump_state(); _exit(71); }

/* ------------------------------------------------------------ system space
 * scb, nvic, systick, dwt, fpu registers. plain storage, with the id registers qemu
 * reports. every address read is logged once to stderr so a new one gets checked. */
#define SYSN 4096
static uint32_t sys_addr[SYSN], sys_val[SYSN]; static unsigned sys_n;
static uint32_t *sys_slot(uint32_t a, int create)
{
    for (unsigned k = 0; k < sys_n; k++) if (sys_addr[k] == a) return &sys_val[k];
    if (!create) return 0;
    if (sys_n == SYSN) rt_fatal("system space table full", a);
    sys_addr[sys_n] = a; sys_val[sys_n] = 0;
    return &sys_val[sys_n++];
}
/* registers whose value differs from plain storage in qemu's cortex-m7 (read off qemu with
 * gdb): CCR resets to 0x200 (STKALIGN), and its cache enable bits IC, DC, BP read as zero
 * (qemu models no caches), so the firmware reads back what qemu returns. */
#define SCB_CCR 0xe000ed14u
static void sys_init(void) { *sys_slot(SCB_CCR, 1) = 0x200u; }
uint32_t rt_sys_rd(uint32_t a, int n)
{
    if ((a >> 28) != 0xe) rt_fatal("read of unmapped address", a);
    uint32_t *v = sys_slot(a & ~3u, 0);
    if (!v) {
        static uint32_t logged[256]; static unsigned nl;
        int seen = 0; for (unsigned k = 0; k < nl; k++) if (logged[k] == a) seen = 1;
        if (!seen && nl < 256) { logged[nl++] = a; fprintf(stderr, "recomp: system read %08x (%d bytes), returning 0\n", a, n); }
        return 0;
    }
    return *v >> (8 * (a & 3));
}
void rt_sys_wr(uint32_t a, uint32_t v, int n)
{
    if ((a >> 28) != 0xe) rt_fatal("write to unmapped address", a);
    uint32_t *s = sys_slot(a & ~3u, 1);
    if (n == 4) *s = v;
    else { uint32_t sh = 8 * (a & 3), m = (n == 2 ? 0xffffu : 0xffu) << sh; *s = (*s & ~m) | ((v << sh) & m); }
    if ((a & ~3u) == SCB_CCR) *s &= ~0x00070000u;
}

/* ------------------------------------------------------------ web host
 * -DRT_WEB: these board files go to the page instead of the file system (handles 32..):
 * dac1.raw / dac2.raw audio out (dac1 blocks while the page's ring is full: that paces
 * the firmware to real time), lcd.txt / midiout.txt text, live.in input lines. */
#ifdef RT_WEB
#include <emscripten.h>
EM_JS(void, js_write, (int kind, const uint8_t *p, uint32_t n), { Module.hostWrite(kind, p, n); });
EM_JS(uint32_t, js_read, (int kind, uint8_t *p, uint32_t n), { return Module.hostRead(kind, p, n); });
static const char *web_names[] = { "dac1.raw", "dac2.raw", "lcd.txt", "midiout.txt", "live.in", "params.txt" };
static char web_tag[6];                               /* fh[h] marker for each web file */
#define WEBK(h) ((h) < 64 && fh[h] >= (FILE *)(void *)web_tag && fh[h] < (FILE *)(void *)(web_tag + 6) ? (int)((char *)(void *)fh[h] - web_tag) : -1)
#endif

/* ------------------------------------------------------------- semihosting */
static FILE *fh[64];
static const char *rename_from, *rename_to;
static int sh_errno;
static char cmdline[] = "lxr.elf";                  /* qemu: the -kernel path (run.sh uses lxr.elf) */
static const char features[] = { 'S', 'H', 'F', 'B', 0x3 };   /* exit_extended | stdout_stderr */
static unsigned feat_pos[64];
static char features_tag;                          /* marks the features handle (FILE is opaque in musl) */
#define FEATURES ((FILE *)(void *)&features_tag)

static void cstr(uint32_t a, uint32_t len, char *out, size_t cap)
{
    if (len >= cap) rt_fatal("semihosting string too long", a);
    for (uint32_t k = 0; k < len; k++) out[k] = (char)ld8(a + k);
    out[len] = 0;
}
static uint32_t rt_semihost_(uint32_t op, uint32_t arg);
uint32_t rt_semihost(uint32_t op, uint32_t arg)
{   /* RECOMP_TRACE_SEMI=n: log the first n calls (op, arg block, result) to stderr */
    static long left = -2;
    if (left == -2) left = getenv("RECOMP_TRACE_SEMI") ? atol(getenv("RECOMP_TRACE_SEMI")) : 0;
    uint32_t a0 = 0, a1 = 0, a2 = 0;
    if (left > 0 && arg <= MEMSZ - 12) { a0 = ld32(arg); a1 = ld32(arg + 4); a2 = ld32(arg + 8); }
    uint32_t r = rt_semihost_(op, arg);
    if (left > 0) { left--; fprintf(stderr, "semi op %02x arg %08x [%08x %08x %08x] -> %08x\n", op, arg, a0, a1, a2, r); }
    return r;
}
static uint32_t rt_semihost_(uint32_t op, uint32_t arg)
{
    switch (op) {
    case 0x01: {                                   /* SYS_OPEN */
        char name[512], path[600];
        cstr(ld32(arg), ld32(arg + 8), name, sizeof name);
        uint32_t mode = ld32(arg + 4);
        static const char *modes[] = { "r", "rb", "r+", "r+b", "w", "wb", "w+", "w+b", "a", "ab", "a+", "a+b" };
        /* qemu: the lowest free handle from 1, the features file is an ordinary handle */
        int h; for (h = 1; h < 64 && fh[h]; h++) {}
        if (h == 64) return (uint32_t)-1;
        if (!strcmp(name, ":tt")) { fh[h] = mode < 4 ? stdin : mode < 8 ? stdout : stderr; return (uint32_t)h; }
        if (!strcmp(name, ":semihosting-features")) { fh[h] = FEATURES; feat_pos[h] = 0; return (uint32_t)h; }
#ifdef RT_WEB
        for (int k = 0; k < (int)(sizeof web_names / sizeof web_names[0]); k++)
            if (!strcmp(name, web_names[k])) { fh[h] = (FILE *)(void *)(web_tag + k); return (uint32_t)h; }   /* same handle numbers as qemu */
#endif
        if (rename_from && !strncmp(name, rename_from, strlen(rename_from)))
            snprintf(path, sizeof path, "%s%s", rename_to, name + strlen(rename_from));
        else snprintf(path, sizeof path, "%s", name);
        if (mode > 11) return (uint32_t)-1;
        FILE *f = fopen(path, modes[mode]);
        if (!f) { sh_errno = 2; return (uint32_t)-1; }
        fh[h] = f; return (uint32_t)h;
    }
    case 0x02: {                                   /* SYS_CLOSE */
        uint32_t h = ld32(arg);
        if (h < 64 && fh[h] == FEATURES) { fh[h] = 0; return 0; }
#ifdef RT_WEB
        if (WEBK(h) >= 0) return 0;
#endif
        if (h >= 64 || !fh[h]) return (uint32_t)-1;
        if (fh[h] != stdin && fh[h] != stdout && fh[h] != stderr) fclose(fh[h]);
        fh[h] = 0; return 0;
    }
    case 0x03: putchar(ld8(arg)); return 0;        /* SYS_WRITEC */
    case 0x04: for (uint32_t k = arg; ld8(k); k++) putchar(ld8(k)); return 0;   /* SYS_WRITE0 */
    case 0x05: {                                   /* SYS_WRITE: returns bytes not written */
        uint32_t h = ld32(arg), p = ld32(arg + 4), n = ld32(arg + 8);
#ifdef RT_WEB
        if (WEBK(h) >= 0) { uint8_t *q = hp(p, n ? n : 1); if (q && n) js_write(WEBK(h), q, n); return 0; }
#endif
        if (h >= 64 || !fh[h]) return n;
        uint8_t *src = hp(p, n ? n : 1);
        if (!src && n) rt_fatal("semihosting write from system space", p);
        size_t w = n ? fwrite(src, 1, n, fh[h]) : 0;
        return n - (uint32_t)w;
    }
    case 0x06: {                                   /* SYS_READ: returns bytes not read */
        uint32_t h = ld32(arg), p = ld32(arg + 4), n = ld32(arg + 8);
        if (h < 64 && fh[h] == FEATURES) {
            uint32_t k = 0;
            while (k < n && feat_pos[h] < sizeof features) st8(p + k++, (uint8_t)features[feat_pos[h]++]);
            return n - k;
        }
#ifdef RT_WEB
        if (WEBK(h) >= 0) { uint8_t *q = hp(p, n ? n : 1); return n - (q && n ? js_read(WEBK(h), q, n) : 0); }
#endif
        if (h >= 64 || !fh[h]) return n;
        uint8_t *dst = hp(p, n ? n : 1);
        if (!dst && n) rt_fatal("semihosting read into system space", p);
        size_t r = n ? fread(dst, 1, n, fh[h]) : 0;
        return n - (uint32_t)r;
    }
    case 0x09:                                     /* SYS_ISTTY */
        /* qemu answers isatty() of the host fd: its console is run.sh's redirected stdout
         * and files are files, so never a tty, and SYS_ERRNO then reports ENOTTY (25) */
        sh_errno = 25;
        return 0;
    case 0x0a: {                                   /* SYS_SEEK (absolute) */
        uint32_t h = ld32(arg), pos = ld32(arg + 4);
        if (h < 64 && fh[h] == FEATURES) { feat_pos[h] = pos; return 0; }
        if (h >= 64 || !fh[h]) return (uint32_t)-1;
        return fseek(fh[h], (long)pos, SEEK_SET) ? (uint32_t)-1 : 0;
    }
    case 0x0c: {                                   /* SYS_FLEN */
        uint32_t h = ld32(arg);
        if (h < 64 && fh[h] == FEATURES) return sizeof features;
#ifdef RT_WEB
        if (WEBK(h) >= 0) return 0;
#endif
        if (h >= 64 || !fh[h]) return (uint32_t)-1;
        long cur = ftell(fh[h]); fseek(fh[h], 0, SEEK_END); long len = ftell(fh[h]); fseek(fh[h], cur, SEEK_SET);
        return (uint32_t)len;
    }
    case 0x13: return (uint32_t)sh_errno;          /* SYS_ERRNO */
    case 0x15: {                                   /* SYS_GET_CMDLINE: qemu answer, read via gdb */
        uint32_t buf = ld32(arg), cap = ld32(arg + 4), n = (uint32_t)strlen(cmdline);
        if (n + 1 > cap) return (uint32_t)-1;
        for (uint32_t k = 0; k <= n; k++) st8(buf + k, (uint8_t)cmdline[k]);
        st32(arg + 4, n);
        return 0;
    }
    case 0x16:                                     /* SYS_HEAPINFO: qemu answer, read via gdb */
        st32(ld32(arg) + 0, 0x60000000u); st32(ld32(arg) + 4, 0x61000000u);
        st32(ld32(arg) + 8, 0x61000000u); st32(ld32(arg) + 12, 0x60000000u);
        return 0;
    case 0x18:                                     /* SYS_EXIT */
        fflush(stdout); exit(arg == 0x20026u ? 0 : 1);
    case 0x20:                                     /* SYS_EXIT_EXTENDED */
        fflush(stdout); exit(ld32(arg) == 0x20026u ? (int)ld32(arg + 4) : 1);
    }
    rt_fatal("semihosting op", op);
    return 0;
}

int main(int argc, char **argv)
{
    if (getenv("RECOMP_TIMEOUT")) { signal(SIGALRM, on_alarm); alarm((unsigned)atoi(getenv("RECOMP_TIMEOUT"))); }
    rename_from = getenv("RECOMP_RENAME_FROM");
    rename_to = getenv("RECOMP_RENAME_TO");
    (void)argc; (void)argv;
    for (unsigned k = 0; k < rt_nsegs; k++) {
        uint8_t *p = hp(rt_segs[k].addr, rt_segs[k].len);
        if (!p) rt_fatal("segment outside memory", rt_segs[k].addr);
        memcpy(p, rt_segs[k].data, rt_segs[k].len);
    }
    sys_init();
    R[13] = ld32(0);                               /* vector table: initial sp, reset */
    R[14] = 0xffffffffu;
    rt_call(ld32(4));
    rt_fatal("reset handler returned", ld32(4));
    return 1;
}
