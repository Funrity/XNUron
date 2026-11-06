/* Differential test program: compiled for x86-64 host (reference) and for AArch64
 * (run in the XNUron core). Both must print identical output. No UB, no libc. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef signed char s8; typedef short s16; typedef int s32; typedef long long s64;

#ifdef __aarch64__
static void sys_write(const char *p, u64 n) {
    register u64 x0 __asm__("x0") = 1, x1 __asm__("x1") = (u64)p, x2 __asm__("x2") = n, x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
}
static void sys_exit(int c) {
    register u64 x0 __asm__("x0") = (u64)c, x8 __asm__("x8") = 93;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8));
    for (;;) {}
}
__attribute__((naked, noreturn)) void _start(void) {
    __asm__("bl main\n bl _exitc\n" );
}
void _exitc(int c) { sys_exit(c); }
__attribute__((optnone, used)) void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
__attribute__((optnone, used)) void *memset(void *d, int v, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)v; return d; }
#else
#include <unistd.h>
static void sys_write(const char *p, u64 n) { if (write(1, p, n) < 0) {} }
#endif

static u64 sum;                        /* running checksum of everything */
static void mix(u64 v) { sum = (sum ^ v) * 0x100000001B3ull; sum ^= sum >> 29; }
static void put(const char *s) { u64 n = 0; while (s[n]) n++; sys_write(s, n); }
static void hex(const char *tag, u64 v) {
    char b[40]; int k = 0; put(tag);
    for (int s = 60; s >= 0; s -= 4) b[k++] = "0123456789abcdef"[(v >> s) & 15];
    b[k++] = '\n'; sys_write(b, k);
}

static u64 rng = 0x9E3779B97F4A7C15ull;
static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static u64 rndv(void) {                /* biased towards interesting values */
    u64 r = rnd();
    switch ((r >> 60) & 7) {
    case 0: return 0; case 1: return ~0ull; case 2: return 1ull << (r & 63);
    case 3: return (u64)(s64)(s32)r; case 4: return r & 0xFF; case 5: return 0x8000000000000000ull;
    default: return rnd();
    }
}

static __attribute__((noinline)) void arith(void) {
    for (int k = 0; k < 4000; k++) {
        u64 a = rndv(), b = rndv(); u32 x = (u32)a, y = (u32)b; unsigned sh = (unsigned)(rnd() & 63);
        mix(a + b); mix(a - b); mix(a * b); mix((u64)x + y); mix((u64)(x - y)); mix((u64)x * y);
        mix(a & b); mix(a | b); mix(a ^ b); mix(a & ~b); mix(a | ~b); mix(a ^ ~b);
        mix(a << sh); mix(a >> sh); mix((u64)((s64)a >> sh)); mix((u64)(x << (sh & 31))); mix((u64)(x >> (sh & 31)));
        mix((u64)(u32)((s32)x >> (sh & 31)));
        mix(sh ? (a >> sh) | (a << (64 - sh)) : a);
        mix(b ? a / b : 7); mix(b ? a % b : 8); mix(y ? (u64)(x / y) : 9); mix(y ? (u64)(x % y) : 10);
        if (b && !((s64)a == (s64)0x8000000000000000ll && (s64)b == -1)) { mix((u64)((s64)a / (s64)b)); mix((u64)((s64)a % (s64)b)); }
        if (y && !((s32)x == (s32)0x80000000 && (s32)y == -1)) { mix((u64)(u32)((s32)x / (s32)y)); mix((u64)(u32)((s32)x % (s32)y)); }
        mix(a < b); mix((s64)a < (s64)b); mix(x <= y); mix((s32)x >= (s32)y); mix(a == b); mix(a != b);
        mix(a > b ? a : b); mix((s64)a > (s64)b ? a : b); mix(a ? 1 : 2);
        mix((u64)(s64)(s8)a); mix((u64)(s64)(s16)a); mix((u64)(s64)(s32)a); mix((u64)(u8)a); mix((u64)(u16)a);
        mix((u64)(u32)(s32)(s8)x); mix((u64)(u32)(s16)x);
        mix(__builtin_bswap64(a)); mix(__builtin_bswap32(x)); mix((u64)__builtin_bswap16((u16)a));
        mix(a ? (u64)__builtin_clzll(a) : 64); mix(x ? (u64)__builtin_clz(x) : 32);
        mix(a ? (u64)__builtin_ctzll(a) : 64); mix(x ? (u64)__builtin_ctz(x) : 32);
        mix((u64)(((u64)a * (unsigned __int128)b) >> 64));
        mix((u64)(((__int128)(s64)a * (__int128)(s64)b) >> 64));
        mix((u64)(u32)(((u64)x * y) >> 32)); mix((u64)((s64)(s32)x * (s64)(s32)y));
        mix(a * b + rndv()); mix(rndv() - a * b);
        mix((a >> 3) & 0xFFFF); mix((a << 7) | (b >> 9)); mix((x >> 5) & 0x1FF); mix(((s64)a >> 17) & 0xFFFFFF);
        mix((a & 0xFFFF0000FFFF0000ull) | 0x00FF00FF00FF00FFull); mix(a ^ 0x5555555555555555ull);
        mix(a & 0x7F); mix((u64)(x | 0x80008000u)); mix((u64)(x & 0x3FFF000u));
    }
}

typedef struct { u8 a; s8 b; u16 c; s16 d; u32 e; s32 f; u64 g; } Rec;
static Rec recs[64]; static u8 bytes[512]; static u64 words[64];
static __attribute__((noinline)) void memory(void) {
    for (int k = 0; k < 3000; k++) {
        u32 i = (u32)(rnd() & 63); Rec *r = &recs[i];
        r->a = (u8)rnd(); r->b = (s8)rnd(); r->c = (u16)rnd(); r->d = (s16)rnd(); r->e = (u32)rnd(); r->f = (s32)rnd(); r->g = rnd();
        u32 j = (u32)(rnd() & 63);
        mix(recs[j].a); mix((u64)(s64)recs[j].b); mix(recs[j].c); mix((u64)(s64)recs[j].d);
        mix(recs[j].e); mix((u64)(s64)recs[j].f); mix(recs[j].g);
        bytes[rnd() & 511] = (u8)rnd();
        u32 o = (u32)(rnd() & 255); u64 w = 0;
        for (int q = 0; q < 8; q++) w |= (u64)bytes[o + q] << (8 * q);
        mix(w); words[rnd() & 63] += w; mix(words[rnd() & 63]);
        Rec t = recs[i]; recs[(i + 1) & 63] = t; mix(recs[(i + 1) & 63].g);
    }
}

static __attribute__((noinline)) u64 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static u64 ackish(u32 m, u32 n) { return m == 0 ? n + 1 : (n == 0 ? ackish(m - 1, 1) : ackish(m - 1, ackish(m, n - 1))); }

static __attribute__((noinline)) u64 sw(u32 v, u64 a) {
    switch (v & 15) {
    case 0: return a + 11; case 1: return a * 3; case 2: return a ^ 0xABCD; case 3: return a - 5;
    case 4: return a << 3; case 5: return a >> 2; case 6: return ~a; case 7: return a | 0xF0F0;
    case 8: return a + v; case 9: return a & 0x3F3F; case 10: return a ^ (a >> 7); case 11: return a * a;
    case 12: return -a; case 13: return a + 1; case 14: return (a << 1) | (a >> 63); default: return a;
    }
}
typedef u64 (*fn)(u64);
static u64 f1(u64 x) { return x + 1; } static u64 f2(u64 x) { return x * 7; } static u64 f3(u64 x) { return x ^ (x >> 3); }
static fn tbl[3] = { f1, f2, f3 };

typedef struct { u32 a : 5; u32 b : 11; s32 c : 7; u32 d : 9; } Bits;

static __attribute__((noinline)) void control(void) {
    mix(fib(18)); mix(ackish(2, 3));
    for (int k = 0; k < 3000; k++) { u64 a = rnd(); mix(sw((u32)rnd(), a)); mix(tbl[rnd() % 3](a)); }
    u64 pop = 0; for (int k = 0; k < 500; k++) { u64 v = rnd(); u64 c = 0; while (v) { c += v & 1; v >>= 1; } pop += c; }
    mix(pop);
    for (int k = 0; k < 500; k++) {
        Bits b; u64 r = rnd(); b.a = (u32)r; b.b = (u32)(r >> 5); b.c = (s32)(r >> 16); b.d = (u32)(r >> 23);
        mix(b.a); mix(b.b); mix((u64)(s64)b.c); mix(b.d);
    }
    int n = 0; for (u32 k = 1; k < 2000; k++) { u32 v = k; while (v != 1 && n < 100000) { v = (v & 1) ? 3 * v + 1 : v / 2; n++; } }
    mix((u64)n);
}

static __attribute__((noinline)) void atomics(void) {
    static volatile u32 w32; static volatile u64 w64; static volatile u8 w8;
    for (int k = 0; k < 400; k++) {
        u64 v = rnd();
        __atomic_fetch_add(&w32, (u32)v, __ATOMIC_SEQ_CST); __atomic_fetch_xor(&w64, v, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&w8, (u8)v, __ATOMIC_ACQ_REL);
        u32 exp = w32; __atomic_compare_exchange_n(&w32, &exp, (u32)(v >> 3), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        mix(w32); mix(w64); mix(w8); mix(__atomic_exchange_n(&w64, v, __ATOMIC_SEQ_CST));
        __atomic_store_n(&w32, (u32)v, __ATOMIC_RELEASE); mix(__atomic_load_n(&w32, __ATOMIC_ACQUIRE));
    }
}

int main(void) {
    arith(); hex("arith   ", sum);
    memory(); hex("memory  ", sum);
    control(); hex("control ", sum);
    atomics(); hex("atomics ", sum);
    put("done\n");
#ifdef __aarch64__
    return 0;
#else
    return 0;
#endif
}
#ifndef __aarch64__
#endif
