/* AdvSIMD "three registers of different length" and "vector x indexed element": NEON intrinsics compiled by clang, compared with plain scalar C loops.
 * Exit code 0 = all passed, N = the N-th check failed. */
#include <arm_neon.h>
typedef unsigned char u8; typedef signed char s8; typedef unsigned short u16; typedef short s16; typedef unsigned int u32; typedef int s32; typedef unsigned long u64; typedef long s64;
static u8 A8[16], B8[16]; static u16 A16[8], B16[8]; static u32 A32[4], B32[4]; static s32 S32[4]; static float FA[4], FB[4];
static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8 | seed << 24; }
static void fill(void) {
    for (int i = 0; i < 16; i++) { A8[i] = (u8)rnd(); B8[i] = (u8)rnd(); }
    for (int i = 0; i < 8; i++) { A16[i] = (u16)rnd(); B16[i] = (u16)rnd(); }
    for (int i = 0; i < 4; i++) { A32[i] = rnd(); B32[i] = rnd(); S32[i] = (s32)rnd(); FA[i] = (float)(s32)(rnd() & 0xffff) / 64.0f; FB[i] = (float)(s32)(rnd() & 0xffff) / 32.0f; }
}
#define NOVEC _Pragma("clang loop vectorize(disable) interleave(disable)")
static int t_diff(void) {
    u16 r16[8], e16[8]; u32 r32[4], e32[4]; u64 r64[2], e64[2]; int n;
    /* UADDL / SADDL / USUBL / UADDW / SADDW / UABAL / UABDL / SABDL / UMULL / SMULL / UMLAL / SMLSL (+2) with 8-, 16-, 32-bit sources */
    vst1q_u16(r16, vaddl_u8(vld1_u8(A8), vld1_u8(B8))); NOVEC for (int i = 0; i < 8; i++) e16[i] = (u16)((u16)A8[i] + B8[i]); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 1;
    vst1q_u16(r16, vaddl_high_u8(vld1q_u8(A8), vld1q_u8(B8))); NOVEC for (int i = 0; i < 8; i++) e16[i] = (u16)((u16)A8[8 + i] + B8[8 + i]); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 2;
    vst1q_s16((s16 *)r16, vaddl_s8(vld1_s8((s8 *)A8), vld1_s8((s8 *)B8))); NOVEC for (int i = 0; i < 8; i++) e16[i] = (u16)(s16)((s16)(s8)A8[i] + (s16)(s8)B8[i]); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 3;
    vst1q_u32(r32, vsubl_u16(vld1_u16(A16), vld1_u16(B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)A16[i] - (u32)B16[i]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 4;
    vst1q_u32(r32, vaddw_u16(vld1q_u32(A32), vld1_u16(B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] + B16[i]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 5;
    vst1q_u32(r32, vaddw_high_u16(vld1q_u32(A32), vld1q_u16(B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] + B16[4 + i]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 6;
    vst1q_s32((s32 *)r32, vaddw_s16(vld1q_s32(S32), vld1_s16((s16 *)B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)(S32[i] + (s32)(s16)B16[i]); for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 7;
    vst1q_u64(r64, vaddl_u32(vld1_u32(A32), vld1_u32(B32))); NOVEC for (int i = 0; i < 2; i++) e64[i] = (u64)A32[i] + B32[i]; for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 8;
    vst1q_u64(r64, vsubl_u32(vld1_u32(A32), vld1_u32(B32))); NOVEC for (int i = 0; i < 2; i++) e64[i] = (u64)A32[i] - B32[i]; for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 9;
    vst1q_u16(r16, vabdl_u8(vld1_u8(A8), vld1_u8(B8))); NOVEC for (int i = 0; i < 8; i++) e16[i] = A8[i] > B8[i] ? (u16)(A8[i] - B8[i]) : (u16)(B8[i] - A8[i]); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 10;
    vst1q_s16((s16 *)r16, vabdl_s8(vld1_s8((s8 *)A8), vld1_s8((s8 *)B8))); NOVEC for (int i = 0; i < 8; i++) { s16 x = (s8)A8[i], y = (s8)B8[i]; e16[i] = (u16)(x > y ? x - y : y - x); } for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 11;
    { uint16x8_t acc = vld1q_u16(A16); vst1q_u16(r16, vabal_u8(acc, vld1_u8(A8), vld1_u8(B8))); NOVEC for (int i = 0; i < 8; i++) e16[i] = (u16)(A16[i] + (A8[i] > B8[i] ? A8[i] - B8[i] : B8[i] - A8[i])); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 12; }
    vst1q_u32(r32, vmull_u16(vld1_u16(A16), vld1_u16(B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)A16[i] * B16[i]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 13;
    vst1q_s32((s32 *)r32, vmull_high_s16(vld1q_s16((s16 *)A16), vld1q_s16((s16 *)B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)((s32)(s16)A16[4 + i] * (s32)(s16)B16[4 + i]); for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 14;
    vst1q_u64(r64, vmull_u32(vld1_u32(A32), vld1_u32(B32))); NOVEC for (int i = 0; i < 2; i++) e64[i] = (u64)A32[i] * B32[i]; for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 15;
    vst1q_s64((s64 *)r64, vmull_s32(vld1_s32(S32), vld1_s32(S32 + 2))); NOVEC for (int i = 0; i < 2; i++) e64[i] = (u64)((s64)S32[i] * (s64)S32[2 + i]); for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 16;
    { uint32x4_t acc = vld1q_u32(A32); vst1q_u32(r32, vmlal_u16(acc, vld1_u16(A16), vld1_u16(B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] + (u32)A16[i] * B16[i]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 17; }
    { int32x4_t acc = vld1q_s32(S32); vst1q_s32((s32 *)r32, vmlsl_s16(acc, vld1_s16((s16 *)A16), vld1_s16((s16 *)B16))); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)(S32[i] - (s32)(s16)A16[i] * (s32)(s16)B16[i]); for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 18; }
    { u8 r8[16], e8[16]; /* ADDHN / RADDHN / SUBHN narrow-high */
      vst1_u8(r8, vaddhn_u16(vld1q_u16(A16), vld1q_u16(B16))); NOVEC for (int i = 0; i < 8; i++) e8[i] = (u8)(((u16)(A16[i] + B16[i])) >> 8); for (n = 0; n < 8; n++) if (r8[n] != e8[n]) return 19;
      vst1_u8(r8, vraddhn_u16(vld1q_u16(A16), vld1q_u16(B16))); NOVEC for (int i = 0; i < 8; i++) e8[i] = (u8)(((u16)(A16[i] + B16[i] + 0x80)) >> 8); for (n = 0; n < 8; n++) if (r8[n] != e8[n]) return 20;
      vst1_u8(r8, vsubhn_u16(vld1q_u16(A16), vld1q_u16(B16))); NOVEC for (int i = 0; i < 8; i++) e8[i] = (u8)(((u16)(A16[i] - B16[i])) >> 8); for (n = 0; n < 8; n++) if (r8[n] != e8[n]) return 21; }
    return 0;
}
static int t_idx(void) {
    u16 r16[8], e16[8]; u32 r32[4], e32[4]; u64 r64[2], e64[2]; float rf[4], ef[4]; int n;
    vst1q_u16(r16, vmulq_laneq_u16(vld1q_u16(A16), vld1q_u16(B16), 5)); NOVEC for (int i = 0; i < 8; i++) e16[i] = (u16)(A16[i] * B16[5]); for (n = 0; n < 8; n++) if (r16[n] != e16[n]) return 30;
    vst1q_u32(r32, vmulq_laneq_u32(vld1q_u32(A32), vld1q_u32(B32), 3)); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] * B32[3]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 31;
    vst1q_u32(r32, vmlaq_laneq_u32(vld1q_u32(A32), vld1q_u32(B32), vld1q_u32(B32), 1)); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] + B32[i] * B32[1]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 32;
    vst1q_u32(r32, vmlsq_laneq_u32(vld1q_u32(A32), vld1q_u32(B32), vld1q_u32(A32), 2)); NOVEC for (int i = 0; i < 4; i++) e32[i] = A32[i] - B32[i] * A32[2]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 33;
    vst1q_u32(r32, vmull_laneq_u16(vld1_u16(A16), vld1q_u16(B16), 6)); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)A16[i] * B16[6]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 34;
    vst1q_u32(r32, vmull_high_laneq_u16(vld1q_u16(A16), vld1q_u16(B16), 2)); NOVEC for (int i = 0; i < 4; i++) e32[i] = (u32)A16[4 + i] * B16[2]; for (n = 0; n < 4; n++) if (r32[n] != e32[n]) return 35;
    vst1q_u64(r64, vmull_laneq_u32(vld1_u32(A32), vld1q_u32(B32), 3)); NOVEC for (int i = 0; i < 2; i++) e64[i] = (u64)A32[i] * B32[3]; for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 36;
    { uint64x2_t acc = vld1q_u64((u64 *)A32); vst1q_u64(r64, vmlal_laneq_u32(acc, vld1_u32(B32), vld1q_u32(A32), 1)); u64 a0[2]; __builtin_memcpy(a0, A32, 16); NOVEC for (int i = 0; i < 2; i++) e64[i] = a0[i] + (u64)B32[i] * A32[1]; for (n = 0; n < 2; n++) if (r64[n] != e64[n]) return 37; }
    vst1q_f32(rf, vmulq_laneq_f32(vld1q_f32(FA), vld1q_f32(FB), 2)); NOVEC for (int i = 0; i < 4; i++) ef[i] = FA[i] * FB[2]; for (n = 0; n < 4; n++) if (rf[n] != ef[n]) return 38;
    vst1q_f32(rf, vfmaq_laneq_f32(vld1q_f32(FA), vld1q_f32(FB), vld1q_f32(FA), 3)); NOVEC for (int i = 0; i < 4; i++) ef[i] = __builtin_fmaf(FB[i], FA[3], FA[i]); for (n = 0; n < 4; n++) if (rf[n] != ef[n]) return 39;
    vst1q_f32(rf, vfmsq_laneq_f32(vld1q_f32(FA), vld1q_f32(FB), vld1q_f32(FA), 0)); NOVEC for (int i = 0; i < 4; i++) ef[i] = __builtin_fmaf(-FB[i], FA[0], FA[i]); for (n = 0; n < 4; n++) if (rf[n] != ef[n]) return 40;
    { double da[2] = { 1.5, -2.25 }, db[2] = { 3.0, 0.5 }, rd[2]; vst1q_f64(rd, vfmaq_laneq_f64(vld1q_f64(da), vld1q_f64(db), vld1q_f64(db), 1)); for (n = 0; n < 2; n++) if (rd[n] != __builtin_fma(db[n], db[1], da[n])) return 41;
      vst1q_f64(rd, vmulq_laneq_f64(vld1q_f64(da), vld1q_f64(db), 0)); for (n = 0; n < 2; n++) if (rd[n] != da[n] * db[0]) return 42;
      /* 0.4.51: scalar by-element forms (fmul d, d, v.d[1] is what the iPhone XS kernel executes in the backlight code) */
      if (vmuld_laneq_f64(da[0], vld1q_f64(db), 1) != da[0] * db[1]) return 43;
      if (vmuld_laneq_f64(da[1], vld1q_f64(db), 0) != da[1] * db[0]) return 44;
      if (vfmad_laneq_f64(da[0], da[1], vld1q_f64(db), 1) != __builtin_fma(da[1], db[1], da[0])) return 45;
      if (vfmsd_laneq_f64(da[0], da[1], vld1q_f64(db), 0) != __builtin_fma(-da[1], db[0], da[0])) return 46;
      { double x = da[0], y = da[1], z = da[0]; float64x2_t v = vld1q_f64(db);   /* the compiler does not emit the D forms itself, so the exact instruction word is forced */
        __asm__ volatile("fmul %d0, %d1, %2.d[1]" : "=w"(z) : "w"(x), "w"(v)); if (z != da[0] * db[1]) return 49;
        z = y; __asm__ volatile("fmla %d0, %d1, %2.d[1]" : "+w"(z) : "w"(x), "w"(v)); if (z != __builtin_fma(da[0], db[1], da[1])) return 50;
        z = y; __asm__ volatile("fmls %d0, %d1, %2.d[0]" : "+w"(z) : "w"(x), "w"(v)); if (z != __builtin_fma(-da[0], db[0], da[1])) return 51; }
      if (vmuls_laneq_f32(FA[1], vld1q_f32(FB), 3) != FA[1] * FB[3]) return 47;
      if (vfmas_laneq_f32(FA[2], FA[0], vld1q_f32(FB), 2) != __builtin_fmaf(FA[0], FB[2], FA[2])) return 48; }
    return 0;
}
int cmain(void) {
    for (int round = 0; round < 6; round++) { fill(); int r = t_diff(); if (r) return r; r = t_idx(); if (r) return r; }
    return 0;
}
