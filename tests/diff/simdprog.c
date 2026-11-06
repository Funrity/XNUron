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


#define N 64
static u8 a8[N], b8[N], r8[N]; static u16 a16[N], b16[N], r16[N]; static u32 a32[N], b32[N], r32[N]; static u64 a64[N], b64[N], r64[N];
static s8 sa8[N], sb8[N]; static s16 sa16[N], sb16[N]; static s32 sa32[N], sb32[N]; static s64 sa64[N], sb64[N];
static float fa[N], fb[N], fr[N]; static double da[N], db[N], dr[N];
static void fill(void) {
    for (int i = 0; i < N; i++) {
        u64 r = rndv(); a8[i] = (u8)r; sa8[i] = (s8)r; a16[i] = (u16)(r >> 3); sa16[i] = (s16)(r >> 3); a32[i] = (u32)(r >> 5); sa32[i] = (s32)(r >> 5); a64[i] = r; sa64[i] = (s64)r;
        r = rndv(); b8[i] = (u8)r; sb8[i] = (s8)r; b16[i] = (u16)(r >> 3); sb16[i] = (s16)(r >> 3); b32[i] = (u32)(r >> 5); sb32[i] = (s32)(r >> 5); b64[i] = r; sb64[i] = (s64)r;
        s32 x = (s32)(rnd() % 2000001) - 1000000, y = (s32)(rnd() % 1999) + 1; if (y > 999) y = y - 1000 + 1; if (y == 0) y = 3;
        fa[i] = (float)x / 7.0f; fb[i] = (float)y / 3.0f; da[i] = (double)x / 7.0; db[i] = (double)y / 3.0;
    }
}
static void mixv(const void *p, int bytes) { const u8 *b = p; for (int i = 0; i < bytes; i += 8) { u64 v = 0; for (int k = 0; k < 8 && i + k < bytes; k++) v |= (u64)b[i + k] << (8 * k); mix(v); } }

static __attribute__((noinline)) void vint(void) {
    for (int rep = 0; rep < 12; rep++) {
        fill();
        for (int i = 0; i < N; i++) r8[i] = a8[i] + b8[i];   mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = a8[i] - b8[i];   mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = a8[i] & ~b8[i];  mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = a8[i] ^ b8[i];   mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = a8[i] | b8[i];   mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = ~a8[i];          mixv(r8, N);
        for (int i = 0; i < N; i++) r8[i] = (u8)__builtin_popcount(a8[i]); mixv(r8, N);
        for (int i = 0; i < N; i++) r16[i] = a16[i] * b16[i]; mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r16[i] = (u16)(a16[i] << 3); mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r16[i] = (u16)(a16[i] >> 5); mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r16[i] = (u16)(sa16[i] >> 5); mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r32[i] = a32[i] + b32[i] * 3; mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r32[i] = sa32[i] < sb32[i] ? a32[i] : b32[i]; mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r32[i] = a32[i] > b32[i] ? a32[i] : b32[i]; mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r32[i] = sa32[i] > 0 ? (u32)sa32[i] : (u32)(-(s64)sa32[i]); mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r32[i] = (a32[i] == b32[i]) ? 0xFFFFFFFFu : (a32[i] < b32[i] ? 1u : 2u); mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r32[i] = __builtin_bswap32(a32[i]); mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r64[i] = a64[i] + b64[i]; mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r64[i] = a64[i] - b64[i]; mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r64[i] = a64[i] << 7; mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r64[i] = __builtin_bswap64(a64[i]); mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r16[i] = (u16)a8[i]; mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r16[i] = (u16)(s16)sa8[i]; mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r32[i] = (u32)a16[i]; mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r64[i] = (u64)(s64)sa32[i]; mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r8[i] = (u8)a16[i]; mixv(r8, N);
        for (int i = 0; i < N; i++) r16[i] = (u16)a32[i]; mixv(r16, 2 * N);
        for (int i = 0; i < N; i++) r32[i] = (u32)a64[i]; mixv(r32, 4 * N);
        u32 s = 0; for (int i = 0; i < N; i++) s += a32[i]; mix(s);
        u16 s16v = 0; for (int i = 0; i < N; i++) s16v += a16[i]; mix(s16v);
        u64 s64v = 0; for (int i = 0; i < N; i++) s64v += a64[i]; mix(s64v);
        s32 mx = sa32[0]; for (int i = 1; i < N; i++) mx = sa32[i] > mx ? sa32[i] : mx; mix((u64)(u32)mx);
        u8 mn = a8[0]; for (int i = 1; i < N; i++) mn = a8[i] < mn ? a8[i] : mn; mix(mn);
        for (int i = 0; i < N; i++) r8[i] = a8[N - 1 - i]; mixv(r8, N);
        for (int i = 0; i < N; i += 2) { r16[i] = a16[i + 1]; r16[i + 1] = a16[i]; } mixv(r16, 2 * N);
    }
}

static __attribute__((noinline)) void vfloat(void) {
    for (int rep = 0; rep < 12; rep++) {
        fill();
        for (int i = 0; i < N; i++) fr[i] = fa[i] + fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] - fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] * fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] / fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = __builtin_sqrtf(fb[i] < 0 ? -fb[i] : fb[i]); mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] < 0 ? -fa[i] : fa[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = -fa[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] > fb[i] ? fa[i] : fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = fa[i] < fb[i] ? fa[i] : fb[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) dr[i] = da[i] + db[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) dr[i] = da[i] * db[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) dr[i] = da[i] / db[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) dr[i] = __builtin_sqrt(db[i] < 0 ? -db[i] : db[i]); mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) dr[i] = (double)fa[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) fr[i] = (float)da[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) fr[i] = (float)sa32[i]; mixv(fr, 4 * N);
        for (int i = 0; i < N; i++) dr[i] = (double)sa64[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) dr[i] = (double)a64[i]; mixv(dr, 8 * N);
        for (int i = 0; i < N; i++) r32[i] = (u32)(s32)fa[i]; mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) r64[i] = (u64)(s64)da[i]; mixv(r64, 8 * N);
        for (int i = 0; i < N; i++) r32[i] = (u32)(fb[i] < 0 ? 0 : fb[i]); mixv(r32, 4 * N);
        for (int i = 0; i < N; i++) { mix(fa[i] < fb[i]); mix(fa[i] == fb[i]); mix(da[i] >= db[i]); }
        for (int i = 0; i < N; i++) fr[i] = fa[i] * 2.5f + 1.0f; mixv(fr, 4 * N);
        float fs = 0; for (int i = 0; i < N; i++) fs += fa[i]; mixv(&fs, 4);
        double ds = 0; for (int i = 0; i < N; i++) ds += da[i] * db[i]; mixv(&ds, 8);
    }
}

static __attribute__((noinline)) void vmem(void) {
    struct S { u64 a[9]; u32 b[5]; } s1, s2;
    for (int rep = 0; rep < 40; rep++) {
        for (int i = 0; i < 9; i++) s1.a[i] = rnd(); for (int i = 0; i < 5; i++) s1.b[i] = (u32)rnd();
        s2 = s1; s2.a[3] ^= 1; mixv(s2.a, sizeof s2.a); mixv(s2.b, sizeof s2.b);
        u8 buf[200]; for (int i = 0; i < 200; i++) buf[i] = (u8)i;
        __builtin_memcpy(buf + 3, buf + 100, 77); mixv(buf, 200);
        __builtin_memset(buf, (int)(rnd() & 0xFF), 130); mixv(buf, 200);
    }
}

int main(void) {
    vint(); hex("vint    ", sum);
    vfloat(); hex("vfloat  ", sum);
    vmem(); hex("vmem    ", sum);
    put("done\n");
    return 0;
}
