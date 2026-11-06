/* Armv8 crypto extension test: real SHA-1 / SHA-256 / AES computed with the NEON crypto intrinsics, compared with the published test vectors
 * (FIPS 180 "abc", FIPS 197 appendix C.1). Exit code 0 = all passed, N = the N-th check failed. */
#include <arm_neon.h>
typedef unsigned int u32; typedef unsigned char u8;
static const u8 abc[3] = { 'a', 'b', 'c' };
static void block_be(u32 *w) { for (int i = 0; i < 16; i++) w[i] = 0; w[0] = ((u32)abc[0] << 24) | ((u32)abc[1] << 16) | ((u32)abc[2] << 8) | 0x80u; w[15] = 24; }
static int sha1_abc(u32 *out) {
    u32 w[16]; block_be(w);
    uint32x4_t W[20]; for (int i = 0; i < 4; i++) W[i] = vld1q_u32(w + 4 * i);
    for (int j = 4; j < 20; j++) W[j] = vsha1su1q_u32(vsha1su0q_u32(W[j - 4], W[j - 3], W[j - 2]), W[j - 1]);
    static const u32 K[4] = { 0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u };
    uint32x4_t abcd = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u }, abcd0 = abcd; u32 e0 = 0xC3D2E1F0u, E0 = e0;
    for (int j = 0; j < 20; j++) {
        uint32x4_t wk = vaddq_u32(W[j], vdupq_n_u32(K[j / 5])); u32 e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        int g = j / 5;
        if (g == 0) abcd = vsha1cq_u32(abcd, e0, wk); else if (g == 2) abcd = vsha1mq_u32(abcd, e0, wk); else abcd = vsha1pq_u32(abcd, e0, wk);
        e0 = e1;
    }
    abcd = vaddq_u32(abcd, abcd0); out[0] = vgetq_lane_u32(abcd, 0); out[1] = vgetq_lane_u32(abcd, 1); out[2] = vgetq_lane_u32(abcd, 2); out[3] = vgetq_lane_u32(abcd, 3); out[4] = e0 + E0;
    return 0;
}
static double dsqrt(double x) { return __builtin_sqrt(x); }
static double dcbrt(double p) { double lo = 0, hi = 8; for (int i = 0; i < 200; i++) { double m = (lo + hi) / 2; if (m * m * m <= p) lo = m; else hi = m; } return lo; }
static void sha256_abc(u32 *out) {
    static const int primes[64] = { 2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97,101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,179,181,191,193,197,199,211,223,227,229,233,239,241,251,257,263,269,271,277,281,283,293,307,311 };
    u32 H[8], K[64];
    for (int i = 0; i < 8; i++) { double r = dsqrt((double)primes[i]); H[i] = (u32)((r - (double)(u32)r) * 4294967296.0); }
    for (int i = 0; i < 64; i++) { double r = dcbrt((double)primes[i]); K[i] = (u32)((r - (double)(u32)r) * 4294967296.0); }
    u32 w[16]; block_be(w);
    uint32x4_t W[4]; for (int i = 0; i < 4; i++) W[i] = vld1q_u32(w + 4 * i);
    uint32x4_t abcd = vld1q_u32(H), efgh = vld1q_u32(H + 4), abcd0 = abcd, efgh0 = efgh;
    for (int i = 0; i < 16; i++) {
        if (i >= 4) W[i & 3] = vsha256su1q_u32(vsha256su0q_u32(W[i & 3], W[(i + 1) & 3]), W[(i + 2) & 3], W[(i + 3) & 3]);
        uint32x4_t wk = vaddq_u32(W[i & 3], vld1q_u32(K + 4 * i)), tmp = abcd;
        abcd = vsha256hq_u32(abcd, efgh, wk); efgh = vsha256h2q_u32(efgh, tmp, wk);
    }
    abcd = vaddq_u32(abcd, abcd0); efgh = vaddq_u32(efgh, efgh0); vst1q_u32(out, abcd); vst1q_u32(out + 4, efgh);
}
static u8 sb[256];
static u8 gm(u8 a, u8 b) { u8 p = 0; while (b) { if (b & 1) p ^= a; a = (u8)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); b >>= 1; } return p; }
static void mksbox(void) { for (int x = 0; x < 256; x++) { u8 inv = 0; if (x) for (int y = 1; y < 256; y++) if (gm((u8)x, (u8)y) == 1) { inv = (u8)y; break; } u8 b = inv, r = b; for (int k = 0; k < 4; k++) { b = (u8)((b << 1) | (b >> 7)); r ^= b; } sb[x] = r ^ 0x63; } }
static int aes_c1(void) {
    mksbox();
    u8 rk[11][16]; for (int i = 0; i < 16; i++) rk[0][i] = (u8)i;
    u8 rcon = 1;
    for (int r = 1; r <= 10; r++) {
        u8 t[4] = { sb[rk[r-1][13]] ^ rcon, sb[rk[r-1][14]], sb[rk[r-1][15]], sb[rk[r-1][12]] };
        for (int k = 0; k < 4; k++) rk[r][k] = rk[r-1][k] ^ t[k];
        for (int k = 4; k < 16; k++) rk[r][k] = rk[r-1][k] ^ rk[r][k-4];
        rcon = gm(rcon, 2);
    }
    u8 pt[16], ct[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    for (int i = 0; i < 16; i++) pt[i] = (u8)(i * 0x11);
    uint8x16_t s = vld1q_u8(pt);
    for (int r = 0; r < 9; r++) s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(rk[r])));
    s = veorq_u8(vaeseq_u8(s, vld1q_u8(rk[9])), vld1q_u8(rk[10]));
    u8 o[16]; vst1q_u8(o, s); for (int i = 0; i < 16; i++) if (o[i] != ct[i]) return 1;
    uint8x16_t z = vdupq_n_u8(0); s = vld1q_u8(ct);
    s = vaesdq_u8(s, vld1q_u8(rk[10]));
    for (int r = 9; r >= 1; r--) { s = vaesimcq_u8(veorq_u8(s, vld1q_u8(rk[r]))); s = vaesdq_u8(s, z); }
    s = veorq_u8(s, vld1q_u8(rk[0]));
    vst1q_u8(o, s); for (int i = 0; i < 16; i++) if (o[i] != pt[i]) return 2;
    return 0;
}
typedef unsigned long u64;
static void ref_clmul(u64 a, u64 b, u64 *lo, u64 *hi) { u64 l = 0, h = 0; for (int k = 0; k < 64; k++) if ((b >> k) & 1) { l ^= a << k; if (k) h ^= a >> (64 - k); } *lo = l; *hi = h; }
static int pmull_checks(void) {
    static const u64 v[6] = { 0x1ull, 0x87ull, 0x8000000000000000ull, 0x0123456789abcdefull, 0xfedcba9876543210ull, 0xdeadbeefcafef00dull };
    for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
        u64 lo, hi; ref_clmul(v[i], v[j], &lo, &hi);
        poly128_t r = vmull_p64((poly64_t)v[i], (poly64_t)v[j]); u64 w[2]; __builtin_memcpy(w, &r, 16);
        if (w[0] != lo || w[1] != hi) return 1;
        u64 a2[2] = { v[j], v[i] }, b2[2] = { v[i], v[j] };
        poly128_t r2 = vmull_high_p64(vreinterpretq_p64_u64(vld1q_u64(a2)), vreinterpretq_p64_u64(vld1q_u64(b2))); u64 w2[2]; __builtin_memcpy(w2, &r2, 16);
        if (w2[0] != lo || w2[1] != hi) return 2;
    }
    u8 a[8] = { 1, 0x87, 0xff, 0x53, 0xca, 0x80, 0x11, 0x02 }, b[8] = { 3, 0x87, 0xff, 0xca, 0x53, 0x80, 0x0f, 0x80 };
    poly16x8_t p = vmull_p8(vreinterpret_p8_u8(vld1_u8(a)), vreinterpret_p8_u8(vld1_u8(b))); unsigned short o[8]; __builtin_memcpy(o, &p, 16);
    for (int e = 0; e < 8; e++) { unsigned short x = 0; for (int k = 0; k < 8; k++) if ((b[e] >> k) & 1) x ^= (unsigned short)((unsigned short)a[e] << k); if (o[e] != x) return 3; }
    return 0;
}
int cmain(void) {
    u32 h1[5]; sha1_abc(h1);
    static const u32 e1[5] = { 0xa9993e36u, 0x4706816au, 0xba3e2571u, 0x7850c26cu, 0x9cd0d89du };
    for (int i = 0; i < 5; i++) if (h1[i] != e1[i]) return 1;
    u32 h2[8]; sha256_abc(h2);
    static const u32 e2[8] = { 0xba7816bfu, 0x8f01cfeau, 0x414140deu, 0x5dae2223u, 0xb00361a3u, 0x96177a9cu, 0xb410ff61u, 0xf20015adu };
    for (int i = 0; i < 8; i++) if (h2[i] != e2[i]) return 2;
    int a = aes_c1(); if (a) return 2 + a;
    int pm = pmull_checks(); if (pm) return 10 + pm;
    return 0;
}
