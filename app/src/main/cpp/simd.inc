/* AdvSIMD / floating point subset for XNUron (included by arm64.c).
 * Implemented: vector register file, all FP/SIMD loads and stores (LDR/STR/LDP/STP, LD1-LD4/ST1-ST4 incl. lane and replicate),
 * scalar FP (single/double: arithmetic, compare, convert, FMOV, FCSEL, FMADD...), and the common AdvSIMD data-processing groups
 * (copy, modified immediate, three-same, two-register misc, across lanes, shift by immediate, permutes, EXT, TBL).
 * Everything else reports itself precisely ("unimplemented SIMD/FP instruction") instead of silently doing something wrong.
 * Not modelled: half precision, FPCR rounding modes / flush-to-zero / default-NaN, FPSR exception & saturation flags, FP traps,
 * CPACR_EL1 trapping (FP is always enabled), crypto and dot-product extensions. Host IEEE arithmetic (round to nearest) is used. */
#include <math.h>
#include <stdint.h>

static uint8_t *VB(A64 *c, int r) { return (uint8_t *)c->vr[r]; }
static uint64_t lane_get(const uint8_t *b, int es, int idx) { uint64_t v = 0; memcpy(&v, b + (size_t)idx * (size_t)es, (size_t)es); return v; }
static void lane_set(uint8_t *b, int es, int idx, uint64_t v) { memcpy(b + (size_t)idx * (size_t)es, &v, (size_t)es); }
static int64_t sxl(uint64_t v, int es) { unsigned bits = (unsigned)es * 8; return bits >= 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits); }
static uint64_t lmask(int es) { return es >= 8 ? ~0ull : ((1ull << (es * 8)) - 1); }

static int unimpl_simd(A64 *c, uint32_t i) {
    char m[160];
    snprintf(m, sizeof m, "unimplemented SIMD/FP instruction 0x%08x (XNUron implements only part of AdvSIMD/FP) at pc=0x%llx", i, (unsigned long long)c->pc);
    return fatal(c, m, i);
}

/* ---------------------------------------------------------------- scalar FP engine */
static double fp_d(uint64_t b, int dbl) { if (dbl) { double d; memcpy(&d, &b, 8); return d; } uint32_t w = (uint32_t)b; float f; memcpy(&f, &w, 4); return (double)f; }
static float fp_f(uint64_t b) { uint32_t w = (uint32_t)b; float f; memcpy(&f, &w, 4); return f; }
static uint64_t pk_f(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static uint64_t pk_d(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
enum { F_ADD, F_SUB, F_MUL, F_DIV, F_MAX, F_MIN, F_MAXNM, F_MINNM, F_NMUL, F_ABD };

static double fmax_arm(double a, double b, bool num, bool mx) {
    if (isnan(a) || isnan(b)) {
        if (num) { if (!isnan(a)) return a; if (!isnan(b)) return b; }
        return isnan(a) ? a : b;
    }
    if (a == 0 && b == 0) { bool sa = signbit(a), sb = signbit(b); return mx ? (sa && sb ? a : (sa ? b : a)) : (sa ? a : (sb ? b : a)); }
    return mx ? (a > b ? a : b) : (a < b ? a : b);
}
static uint64_t fp_bin(int op, uint64_t a, uint64_t b, int dbl) {
    if (dbl) {
        double x = fp_d(a, 1), y = fp_d(b, 1), r;
        switch (op) {
        case F_ADD: r = x + y; break; case F_SUB: r = x - y; break; case F_MUL: r = x * y; break; case F_DIV: r = x / y; break;
        case F_MAX: r = fmax_arm(x, y, false, true); break; case F_MIN: r = fmax_arm(x, y, false, false); break;
        case F_MAXNM: r = fmax_arm(x, y, true, true); break; case F_MINNM: r = fmax_arm(x, y, true, false); break;
        case F_NMUL: r = -(x * y); break; default: r = fabs(x - y); break;
        }
        return pk_d(r);
    } else {
        float x = fp_f(a), y = fp_f(b), r;
        switch (op) {
        case F_ADD: r = x + y; break; case F_SUB: r = x - y; break; case F_MUL: r = x * y; break; case F_DIV: r = x / y; break;
        case F_MAX: r = (float)fmax_arm(x, y, false, true); break; case F_MIN: r = (float)fmax_arm(x, y, false, false); break;
        case F_MAXNM: r = (float)fmax_arm(x, y, true, true); break; case F_MINNM: r = (float)fmax_arm(x, y, true, false); break;
        case F_NMUL: r = -(x * y); break; default: r = fabsf(x - y); break;
        }
        return pk_f(r);
    }
}
static void fp_flags(A64 *c, double x, double y) {
    if (isnan(x) || isnan(y)) { c->n = 0; c->z = 0; c->c = 1; c->v = 1; }
    else if (x == y) { c->n = 0; c->z = 1; c->c = 1; c->v = 0; }
    else if (x < y) { c->n = 1; c->z = 0; c->c = 0; c->v = 0; }
    else { c->n = 0; c->z = 0; c->c = 1; c->v = 0; }
}
/* convert to integer with the given rounding (0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away); saturating, NaN -> 0 */
static uint64_t fp_to_int(double x, int bits, bool sgn, int mode) {
    if (isnan(x)) return 0;
    double r;
    switch (mode) { case 0: r = nearbyint(x); break; case 1: r = ceil(x); break; case 2: r = floor(x); break; case 3: r = trunc(x); break; default: r = round(x); break; }
    if (sgn) {
        double lo = -ldexp(1.0, bits - 1), hi = ldexp(1.0, bits - 1);
        if (r >= hi) return bits == 64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
        if (r < lo) return bits == 64 ? 0x8000000000000000ull : (uint64_t)(uint32_t)0x80000000u;
        int64_t v = (int64_t)r; return bits == 64 ? (uint64_t)v : (uint64_t)(uint32_t)(int32_t)v;
    }
    if (r <= 0) return 0;
    double hi = ldexp(1.0, bits);
    if (r >= hi) return bits == 64 ? ~0ull : 0xFFFFFFFFull;
    return (uint64_t)r;
}
static uint64_t vfp_expand_imm(unsigned imm8, int dbl) {
    uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cdefgh = imm8 & 0x3F;
    if (dbl) return a << 63 | (b ^ 1) << 62 | (b ? 0xFFull << 54 : 0) | cdefgh << 48;
    return a << 31 | (b ^ 1) << 30 | (b ? 0x1Full << 25 : 0) | cdefgh << 19;
}
static void vwrite_scalar(A64 *c, int r, int bytes, uint64_t v) { c->vr[r][0] = bytes == 4 ? (v & 0xFFFFFFFFull) : v; c->vr[r][1] = 0; }
static uint64_t vread_scalar(A64 *c, int r, int bytes) { return bytes == 4 ? (c->vr[r][0] & 0xFFFFFFFFull) : c->vr[r][0]; }

static bool cond_holds(const A64 *c, unsigned cond) {
    bool r;
    switch (cond >> 1) {
    case 0: r = c->z; break; case 1: r = c->c; break; case 2: r = c->n; break; case 3: r = c->v; break;
    case 4: r = c->c && !c->z; break; case 5: r = c->n == c->v; break; case 6: r = c->n == c->v && !c->z; break; default: r = true; break;
    }
    return (cond & 1) && cond != 15 ? !r : r;
}

static int simd_fp_scalar(A64 *c, uint32_t i) {
    int rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
    int ft = (i >> 22) & 3;
    if (((i >> 24) & 0x1F) == 0x1F) {                         /* FMADD/FMSUB/FNMADD/FNMSUB */
        if (ft > 1) return unimpl_simd(c, i);
        int dbl = ft, ra = (i >> 10) & 31, o1 = (i >> 21) & 1, o0 = (i >> 15) & 1, by = dbl ? 8 : 4;
        uint64_t ab = vread_scalar(c, ra, by), nb = vread_scalar(c, rn, by), mb = vread_scalar(c, rm, by);
        bool negn = o1 ^ o0, nega = o1;                                  /* FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m */
        if (dbl) {
            double a = fp_d(ab, 1), n = fp_d(nb, 1), m = fp_d(mb, 1);
            vwrite_scalar(c, rd, 8, pk_d(fma(negn ? -n : n, m, nega ? -a : a)));
        } else {
            float a = fp_f(ab), n = fp_f(nb), m = fp_f(mb);
            vwrite_scalar(c, rd, 4, pk_f(fmaf(negn ? -n : n, m, nega ? -a : a)));
        }
        return 0;
    }
    if (!((i >> 21) & 1)) {                                    /* fixed-point conversions: sf 0 S 11110 ftype 0 rmode opcode scale Rn Rd (0.4.26) */
        int sf = (i >> 31) & 1, rmode = (i >> 19) & 3, opc = (i >> 16) & 7, fb = 64 - (int)((i >> 10) & 0x3F);
        if (ft > 1 || (!sf && fb > 32)) return unimpl_simd(c, i);
        int dbl = ft, bits = sf ? 64 : 32;
        if ((opc == 0 || opc == 1) && rmode == 3) {              /* FCVTZS / FCVTZU (scalar, fixed-point) */
            double x = ldexp(fp_d(vread_scalar(c, rn, dbl ? 8 : 4), dbl), fb);
            uint64_t r = fp_to_int(x, bits, opc == 0, 3); W(c, rd, sf ? r : (r & M32), false); return 0;
        }
        if ((opc == 2 || opc == 3) && rmode == 0) {              /* SCVTF / UCVTF (scalar, fixed-point) */
            uint64_t v = R(c, rn, false); double d;
            if (opc == 2) d = (double)(sf ? (int64_t)v : (int64_t)(int32_t)v); else d = (double)(sf ? v : (v & M32));
            d = ldexp(d, -fb); if (dbl) vwrite_scalar(c, rd, 8, pk_d(d)); else vwrite_scalar(c, rd, 4, pk_f((float)d)); return 0;
        }
        return unimpl_simd(c, i);
    }
    /* conversions to/from integer: sf 0 S 11110 ftype 1 rmode opcode 000000 */
    if (((i >> 10) & 0x3F) == 0) {
        int sf = (i >> 31) & 1, rmode = (i >> 19) & 3, opc = (i >> 16) & 7;
        if (opc == 6 || opc == 7) {                             /* FMOV between general and FP registers */
            bool to_gp = opc == 6;
            if (sf == 0 && ft == 0 && rmode == 0) {
                if (to_gp) W(c, rd, vread_scalar(c, rn, 4), false); else vwrite_scalar(c, rd, 4, R(c, rn, false) & M32);
                return 0;
            }
            if (sf == 1 && ft == 1 && rmode == 0) {
                if (to_gp) W(c, rd, c->vr[rn][0], false); else vwrite_scalar(c, rd, 8, R(c, rn, false));
                return 0;
            }
            if (sf == 1 && ft == 2 && rmode == 1) {              /* FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn */
                if (to_gp) W(c, rd, c->vr[rn][1], false); else c->vr[rd][1] = R(c, rn, false);
                return 0;
            }
            return unimpl_simd(c, i);
        }
        if (ft > 1) return unimpl_simd(c, i);
        int dbl = ft, bits = sf ? 64 : 32;
        if (opc == 2 || opc == 3) {                             /* SCVTF / UCVTF (integer source) */
            uint64_t v = R(c, rn, false); bool sg = opc == 2;
            double d; float f;
            if (sg) { int64_t sv = sf ? (int64_t)v : (int64_t)(int32_t)v; d = (double)sv; f = (float)sv; }
            else { uint64_t uv = sf ? v : (v & M32); d = (double)uv; f = (float)uv; }
            if (dbl) vwrite_scalar(c, rd, 8, pk_d(d)); else vwrite_scalar(c, rd, 4, pk_f(f));
            return 0;
        }
        int mode = -1; bool sg = true;
        if (opc == 0) { mode = rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3; sg = true; }
        else if (opc == 1) { mode = rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3; sg = false; }
        else if (opc == 4 && rmode == 0) { mode = 4; sg = true; }
        else if (opc == 5 && rmode == 0) { mode = 4; sg = false; }
        if (mode < 0) return unimpl_simd(c, i);
        uint64_t r = fp_to_int(fp_d(vread_scalar(c, rn, dbl ? 8 : 4), dbl), bits, sg, mode);
        W(c, rd, sf ? r : (r & M32), false);
        return 0;
    }
    if (ft > 1) return unimpl_simd(c, i);
    int dbl = ft, by = dbl ? 8 : 4;
    if (((i >> 10) & 0x1F) == 0x10) {                           /* 1-source */
        int opc = (i >> 15) & 0x3F;
        uint64_t a = vread_scalar(c, rn, by);
        if (opc == 0) { vwrite_scalar(c, rd, by, a); return 0; }                                  /* FMOV */
        if (opc == 1) { vwrite_scalar(c, rd, by, a & ~(dbl ? (1ull << 63) : (1ull << 31))); return 0; }   /* FABS */
        if (opc == 2) { vwrite_scalar(c, rd, by, a ^ (dbl ? (1ull << 63) : (1ull << 31))); return 0; }    /* FNEG */
        if (opc == 3) { if (dbl) vwrite_scalar(c, rd, 8, pk_d(sqrt(fp_d(a, 1)))); else vwrite_scalar(c, rd, 4, pk_f(sqrtf(fp_f(a)))); return 0; }
        if (opc == 4 && dbl) { vwrite_scalar(c, rd, 4, pk_f((float)fp_d(a, 1))); return 0; }      /* FCVT Sd, Dn */
        if (opc == 5 && !dbl) { vwrite_scalar(c, rd, 8, pk_d((double)fp_f(a))); return 0; }      /* FCVT Dd, Sn */
        if (opc >= 8 && opc <= 15) {                                                             /* FRINTN/P/M/Z/A/X/I */
            double x = fp_d(a, dbl), r;
            switch (opc) { case 8: r = nearbyint(x); break; case 9: r = ceil(x); break; case 10: r = floor(x); break; case 11: r = trunc(x); break; case 12: r = round(x); break; default: r = nearbyint(x); break; }
            if (dbl) vwrite_scalar(c, rd, 8, pk_d(r)); else vwrite_scalar(c, rd, 4, pk_f((float)r));
            return 0;
        }
        return unimpl_simd(c, i);
    }
    if (((i >> 10) & 0xF) == 8) {                               /* FCMP / FCMPE */
        double x = fp_d(vread_scalar(c, rn, by), dbl), y = (i & 8) ? 0.0 : fp_d(vread_scalar(c, rm, by), dbl);
        fp_flags(c, x, y); return 0;
    }
    if (((i >> 10) & 7) == 4) {                                 /* FMOV immediate */
        vwrite_scalar(c, rd, by, vfp_expand_imm((i >> 13) & 0xFF, dbl)); return 0;
    }
    switch ((i >> 10) & 3) {
    case 1: {                                                   /* FCCMP / FCCMPE */
        if (cond_holds(c, (i >> 12) & 15)) fp_flags(c, fp_d(vread_scalar(c, rn, by), dbl), fp_d(vread_scalar(c, rm, by), dbl));
        else { unsigned f = i & 15; c->n = (f >> 3) & 1; c->z = (f >> 2) & 1; c->c = (f >> 1) & 1; c->v = f & 1; }
        return 0;
    }
    case 2: {                                                   /* 2-source */
        static const int map[9] = { F_MUL, F_DIV, F_ADD, F_SUB, F_MAX, F_MIN, F_MAXNM, F_MINNM, F_NMUL };
        int opc = (i >> 12) & 15; if (opc > 8) return unimpl_simd(c, i);
        vwrite_scalar(c, rd, by, fp_bin(map[opc], vread_scalar(c, rn, by), vread_scalar(c, rm, by), dbl));
        return 0;
    }
    default:                                                    /* FCSEL */
        vwrite_scalar(c, rd, by, cond_holds(c, (i >> 12) & 15) ? vread_scalar(c, rn, by) : vread_scalar(c, rm, by));
        return 0;
    }
}

/* ---------------------------------------------------------------- vector helpers */
static void vstore_res(A64 *c, int rd, const uint8_t *res, int Q) {
    uint8_t *d = VB(c, rd); memcpy(d, res, Q ? 16 : 8); if (!Q) memset(d + 8, 0, 8);
}
static uint64_t asr_lane(uint64_t v, int es, int sh) { return (uint64_t)(sxl(v, es) >> sh) & lmask(es); }

static int simd_copy(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, op = (i >> 29) & 1, imm5 = (i >> 16) & 31, imm4 = (i >> 11) & 15, rn = (i >> 5) & 31, rd = i & 31;
    if (!(imm5 & 15) && !(imm5 & 16)) return unimpl_simd(c, i);
    int sz = __builtin_ctz((unsigned)imm5); if (sz > 3) return unimpl_simd(c, i);
    int es = 1 << sz, idx = imm5 >> (sz + 1), dsz = Q ? 16 : 8;
    uint8_t res[16] = {0};
    if (op) {                                                   /* INS (element) */
        if (!Q) return unimpl_simd(c, i);
        int idx2 = imm4 >> sz;
        uint8_t *d = VB(c, rd); lane_set(d, es, idx, lane_get(VB(c, rn), es, idx2)); return 0;
    }
    switch (imm4) {
    case 0: { uint64_t v = lane_get(VB(c, rn), es, idx); for (int e = 0; e < dsz / es; e++) lane_set(res, es, e, v); vstore_res(c, rd, res, Q); return 0; }
    case 1: { uint64_t v = R(c, rn, false) & lmask(es); for (int e = 0; e < dsz / es; e++) lane_set(res, es, e, v); vstore_res(c, rd, res, Q); return 0; }
    case 3: lane_set(VB(c, rd), es, idx, R(c, rn, false) & lmask(es)); return 0;
    case 5: { uint64_t v = (uint64_t)sxl(lane_get(VB(c, rn), es, idx), es); W(c, rd, Q ? v : (v & M32), false); return 0; }
    case 7: W(c, rd, lane_get(VB(c, rn), es, idx), false); return 0;
    default: return unimpl_simd(c, i);
    }
}

static int simd_modimm(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, op = (i >> 29) & 1, cmode = (i >> 12) & 15, rd = i & 31;
    unsigned imm8 = (((i >> 16) & 7) << 5) | ((i >> 5) & 31);
    if ((i >> 11) & 1) return unimpl_simd(c, i);                 /* half-precision FMOV */
    uint64_t imm; int mode;                                     /* mode 0 MOVI/MVNI, 1 ORR/BIC */
    int w32 = 0;
    if ((cmode & 9) == 0) { imm = (uint64_t)imm8 << (8 * ((cmode >> 1) & 3)); imm |= imm << 32; mode = 0; w32 = 1; }
    else if ((cmode & 9) == 1) { imm = (uint64_t)imm8 << (8 * ((cmode >> 1) & 3)); imm |= imm << 32; mode = 1; w32 = 1; }
    else if ((cmode & 13) == 8) { imm = (uint64_t)imm8 << (8 * ((cmode >> 1) & 1)); imm |= imm << 16; imm |= imm << 32; mode = 0; }
    else if ((cmode & 13) == 9) { imm = (uint64_t)imm8 << (8 * ((cmode >> 1) & 1)); imm |= imm << 16; imm |= imm << 32; mode = 1; }
    else if ((cmode & 14) == 12) { uint64_t v = cmode & 1 ? ((uint64_t)imm8 << 16) | 0xFFFF : ((uint64_t)imm8 << 8) | 0xFF; imm = v | v << 32; mode = 0; w32 = 1; }
    else if (cmode == 14) {
        if (!op) { imm = 0; for (int b = 0; b < 8; b++) imm |= (uint64_t)imm8 << (8 * b); mode = 0; }
        else { imm = 0; for (int b = 0; b < 8; b++) if ((imm8 >> b) & 1) imm |= 0xFFull << (8 * b); mode = 0; op = 0; }
    } else {                                                    /* cmode 1111: FMOV */
        if (!op) { uint64_t v = vfp_expand_imm(imm8, 0); imm = v | v << 32; }
        else { if (!Q) return unimpl_simd(c, i); imm = vfp_expand_imm(imm8, 1); }
        mode = 0; op = 0;
    }
    (void)w32;
    uint64_t r0, r1, *d = c->vr[rd];
    if (mode == 0) { uint64_t v = op ? ~imm : imm; r0 = v; r1 = Q ? v : 0; }
    else { uint64_t v = imm; r0 = op ? d[0] & ~v : d[0] | v; r1 = Q ? (op ? d[1] & ~v : d[1] | v) : 0; }
    d[0] = r0; d[1] = r1;
    return 0;
}

static int simd_3same_fp(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, U = (i >> 29) & 1, hi = (i >> 23) & 1, dbl = (i >> 22) & 1, op = (i >> 11) & 31;
    int rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
    if (dbl && !Q) return unimpl_simd(c, i);
    int es = dbl ? 8 : 4, n = (Q ? 16 : 8) / es;
    uint8_t A[16], B[16], res[16] = {0}, D[16];
    memcpy(A, VB(c, rn), 16); memcpy(B, VB(c, rm), 16); memcpy(D, VB(c, rd), 16);
    int bop = -1; bool pair = false; int cmp = 0;                /* cmp: 1 eq, 2 ge, 3 gt, 4 acge, 5 acgt */
    bool fma_ = false;
    if (!U) {
        switch (op) {
        case 0x18: bop = hi ? F_MINNM : F_MAXNM; break;
        case 0x19: fma_ = true; break;
        case 0x1A: bop = hi ? F_SUB : F_ADD; break;
        case 0x1C: if (hi) return unimpl_simd(c, i); cmp = 1; break;
        case 0x1E: bop = hi ? F_MIN : F_MAX; break;
        default: return unimpl_simd(c, i);
        }
    } else {
        switch (op) {
        case 0x18: bop = hi ? F_MINNM : F_MAXNM; pair = true; break;
        case 0x1A: if (hi) bop = F_ABD; else { bop = F_ADD; pair = true; } break;
        case 0x1B: if (hi) return unimpl_simd(c, i); bop = F_MUL; break;
        case 0x1C: cmp = hi ? 3 : 2; break;
        case 0x1D: cmp = hi ? 5 : 4; break;
        case 0x1E: bop = hi ? F_MIN : F_MAX; pair = true; break;
        case 0x1F: if (hi) return unimpl_simd(c, i); bop = F_DIV; break;
        default: return unimpl_simd(c, i);
        }
    }
    for (int e = 0; e < n; e++) {
        uint64_t a, b, r;
        if (pair) { int k = 2 * e; if (k < n) { a = lane_get(A, es, k); b = lane_get(A, es, k + 1); } else { a = lane_get(B, es, k - n); b = lane_get(B, es, k - n + 1); } }
        else { a = lane_get(A, es, e); b = lane_get(B, es, e); }
        if (cmp) {
            double x = fp_d(a, dbl), y = fp_d(b, dbl);
            if (cmp >= 4) { x = fabs(x); y = fabs(y); }
            bool t = cmp == 1 ? x == y : (cmp == 2 || cmp == 4) ? x >= y : x > y;
            r = t ? lmask(es) : 0;
        } else if (fma_) {
            uint64_t acc = lane_get(D, es, e);
            if (dbl) { double p = hi ? -fp_d(a, 1) : fp_d(a, 1); r = pk_d(fma(p, fp_d(b, 1), fp_d(acc, 1))); }
            else { float p = hi ? -fp_f(a) : fp_f(a); r = pk_f(fmaf(p, fp_f(b), fp_f(acc))); }
        } else r = fp_bin(bop, a, b, dbl);
        lane_set(res, es, e, r);
    }
    vstore_res(c, rd, res, Q);
    return 0;
}

/* saturating add/sub on one lane (a,b zero-extended, sa,sb sign-extended, es bytes) */
static uint64_t sat_addsub(bool uns, bool sub, uint64_t a, uint64_t b, int64_t sa, int64_t sb, int es) {
    int bits = es * 8;
    if (uns) {
        if (sub) return a > b ? a - b : 0;
        uint64_t r;
        if (es == 8) return __builtin_add_overflow(a, b, &r) ? ~0ull : r;
        r = a + b; return r > lmask(es) ? lmask(es) : r;
    }
    int64_t r, hi = bits == 64 ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1), lo = bits == 64 ? INT64_MIN : -hi - 1;
    if (es == 8) { bool o = sub ? __builtin_sub_overflow(sa, sb, &r) : __builtin_add_overflow(sa, sb, &r); return (uint64_t)(o ? (sa < 0 ? lo : hi) : r); }
    r = sub ? sa - sb : sa + sb;
    return (uint64_t)(r > hi ? hi : r < lo ? lo : r);
}

static int simd_3same(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, U = (i >> 29) & 1, size = (i >> 22) & 3, op = (i >> 11) & 31;
    int rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
    uint8_t A[16], B[16], D[16], res[16] = {0};
    memcpy(A, VB(c, rn), 16); memcpy(B, VB(c, rm), 16); memcpy(D, VB(c, rd), 16);
    int dsz = Q ? 16 : 8;
    if (op == 3) {                                              /* logic */
        for (int k = 0; k < dsz; k++) {
            uint8_t a = A[k], b = B[k], d = D[k], r;
            if (!U) r = size == 0 ? (a & b) : size == 1 ? (a & ~b) : size == 2 ? (a | b) : (a | ~b);
            else r = size == 0 ? (a ^ b) : size == 1 ? (uint8_t)((d & a) | (~d & b)) : size == 2 ? (uint8_t)(d ^ ((d ^ a) & b)) : (uint8_t)(d ^ ((d ^ a) & ~b));
            res[k] = r;
        }
        vstore_res(c, rd, res, Q); return 0;
    }
    if (op >= 0x18) return simd_3same_fp(c, i);
    int es = 1 << size, n = dsz / es; int bits = es * 8;
    if (size == 3 && !Q) return unimpl_simd(c, i);
    for (int e = 0; e < n; e++) {
        uint64_t a, b, d = lane_get(D, es, e), r = 0;
        bool pairwise = op == 0x14 || op == 0x15 || op == 0x17;
        if (pairwise) { int k = 2 * e; if (k < n) { a = lane_get(A, es, k); b = lane_get(A, es, k + 1); } else { a = lane_get(B, es, k - n); b = lane_get(B, es, k - n + 1); } }
        else { a = lane_get(A, es, e); b = lane_get(B, es, e); }
        int64_t sa = sxl(a, es), sb = sxl(b, es);
        switch (op) {
        case 0x00: r = U ? (a & b) + ((a ^ b) >> 1) : (uint64_t)((sa & sb) + ((sa ^ sb) >> 1)); break;                        /* SHADD / UHADD (no 128-bit types: armv7 builds) */
        case 0x01: r = sat_addsub(U, false, a, b, sa, sb, es); break;                                                               /* SQADD / UQADD */
        case 0x02: r = U ? (a | b) - ((a ^ b) >> 1) : (uint64_t)((sa | sb) - ((sa ^ sb) >> 1)); break;                              /* SRHADD / URHADD */
        case 0x04: r = U ? (a >> 1) - (b >> 1) - ((~a & b) & 1) : (uint64_t)((sa >> 1) - (sb >> 1) - ((~sa & sb) & 1)); break;     /* SHSUB / UHSUB */
        case 0x05: r = sat_addsub(U, true, a, b, sa, sb, es); break;                                                                /* SQSUB / UQSUB */
        case 0x06: r = (U ? a > b : sa > sb) ? lmask(es) : 0; break;
        case 0x07: r = (U ? a >= b : sa >= sb) ? lmask(es) : 0; break;
        case 0x08: { int sh = (int8_t)(b & 0xFF);
                     if (sh >= 0) r = sh >= bits ? 0 : (a << sh);
                     else { int rs = -sh; r = U ? (rs >= bits ? 0 : a >> rs) : (uint64_t)(sa >> (rs >= bits ? bits - 1 : rs)); } break; }
        case 0x0C: r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb); break;
        case 0x0D: r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb); break;
        case 0x0E: r = U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break;
        case 0x0F: r = d + (U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break;
        case 0x10: r = U ? a - b : a + b; break;
        case 0x11: r = U ? (a == b ? lmask(es) : 0) : ((a & b) ? lmask(es) : 0); break;
        case 0x12: r = U ? d - a * b : d + a * b; break;
        case 0x13: if (U) return unimpl_simd(c, i); r = a * b; break;
        case 0x14: r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb); break;
        case 0x15: r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb); break;
        case 0x17: if (U) return unimpl_simd(c, i); r = a + b; break;
        default: return unimpl_simd(c, i);
        }
        lane_set(res, es, e, r & lmask(es));
    }
    vstore_res(c, rd, res, Q);
    return 0;
}

static int simd_2misc(A64 *c, uint32_t i, bool scalar) {
    int Q = scalar ? 1 : (i >> 30) & 1, U = (i >> 29) & 1, size = (i >> 22) & 3, op = (i >> 12) & 31;
    int rn = (i >> 5) & 31, rd = i & 31;
    uint8_t A[16], res[16] = {0};
    memcpy(A, VB(c, rn), 16);
    int dsz = scalar ? 8 : (Q ? 16 : 8);
    int es = 1 << size;
    int n = scalar ? 1 : dsz / es;
    if (scalar && size != 3 && op != 0x1D && op != 0x1B && op != 0x1A && op != 0x1C) return unimpl_simd(c, i);
    switch (op) {
    case 0x00: case 0x01: {                                      /* REV64 / REV32 (U) / REV16 */
        int grp = op == 1 ? 2 : (U ? 4 : 8); if (grp <= es) return unimpl_simd(c, i);
        for (int b = 0; b < dsz; b++) { int base = b / grp * grp, off = b % grp; res[base + (grp - es - (off / es) * es) + (off % es)] = A[b]; }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x05:
        if (!U) { if (size) return unimpl_simd(c, i); for (int b = 0; b < dsz; b++) res[b] = (uint8_t)__builtin_popcount(A[b]); }
        else if (size == 0) for (int b = 0; b < dsz; b++) res[b] = (uint8_t)~A[b];
        else if (size == 1) for (int b = 0; b < dsz; b++) { uint8_t x = A[b], r = 0; for (int k = 0; k < 8; k++) r |= (uint8_t)(((x >> k) & 1) << (7 - k)); res[b] = r; }
        else return unimpl_simd(c, i);
        vstore_res(c, rd, res, Q); return 0;
    case 0x08: case 0x09: case 0x0A: case 0x0B: {                /* CMGT/CMGE/CMEQ/CMLT/CMLE zero, ABS, NEG */
        for (int e = 0; e < n; e++) {
            int64_t s = sxl(lane_get(A, es, e), es); uint64_t r;
            if (op == 0x0B) r = (uint64_t)(U ? -s : (s < 0 ? -s : s));
            else {
                bool t = op == 0x08 ? (U ? s >= 0 : s > 0) : op == 0x09 ? (U ? s <= 0 : s == 0) : (s < 0);
                if (op == 0x0A && U) return unimpl_simd(c, i);
                r = t ? lmask(es) : 0;
            }
            lane_set(res, es, e, r & lmask(es));
        }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x12: case 0x14: {                                      /* XTN / XTN2 (SQXTN etc. not modelled) */
        if (op == 0x12 && U) return unimpl_simd(c, i);
        if (op == 0x14) return unimpl_simd(c, i);
        int ds = Q ? 8 : 8; int ne = ds / es;                    /* source lanes of width 2*es */
        uint8_t *d = VB(c, rd); uint8_t tmp[16] = {0};
        int ses = es * 2; if (ses > 8) return unimpl_simd(c, i);
        for (int e = 0; e < ne; e++) lane_set(tmp, es, e, lane_get(A, ses, e) & lmask(es));
        if (Q) memcpy(d + 8, tmp, 8); else { memcpy(d, tmp, 8); memset(d + 8, 0, 8); }
        return 0;
    }
    case 0x16: case 0x17: {                                      /* FCVTN(2) / FCVTL(2): double <-> single only */
        if (U || !(size & 1) || scalar) return unimpl_simd(c, i);
        uint8_t *d = VB(c, rd);
        if (op == 0x17) { for (int e = 0; e < 2; e++) lane_set(res, 8, e, pk_d((double)fp_f(lane_get(A + (Q ? 8 : 0), 4, e)))); memcpy(d, res, 16); }
        else { uint8_t tmp[16] = {0}; for (int e = 0; e < 2; e++) lane_set(tmp, 4, e, pk_f((float)fp_d(lane_get(A, 8, e), 1)));
               if (Q) memcpy(d + 8, tmp, 8); else { memcpy(d, tmp, 8); memset(d + 8, 0, 8); } }
        return 0;
    }
    case 0x0C: case 0x0D: case 0x0E: {                           /* FCMGT/FCMGE, FCMEQ/FCMLE, FCMLT against zero (size<1>=1) */
        if (!(size & 2)) return unimpl_simd(c, i);
        int dbl = size & 1, fes = dbl ? 8 : 4; int cnt = scalar ? 1 : dsz / fes;
        for (int e = 0; e < cnt; e++) {
            double x = fp_d(lane_get(A, fes, e), dbl); bool t;
            if (op == 0x0C) t = U ? x >= 0 : x > 0; else if (op == 0x0D) t = U ? x <= 0 : x == 0; else { if (U) return unimpl_simd(c, i); t = x < 0; }
            lane_set(res, fes, e, t ? lmask(fes) : 0);
        }
        if (scalar) { c->vr[rd][0] = lane_get(res, fes, 0); c->vr[rd][1] = 0; return 0; }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x0F: {                                                 /* FABS / FNEG (size<1>=1) */
        if (!(size & 2)) return unimpl_simd(c, i);
        int dbl = size & 1, fes = dbl ? 8 : 4; uint64_t sb = dbl ? (1ull << 63) : (1ull << 31);
        for (int e = 0; e < dsz / fes; e++) { uint64_t v = lane_get(A, fes, e); lane_set(res, fes, e, U ? v ^ sb : v & ~sb); }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x1F: {                                                 /* FSQRT (U=1) */
        if (!U || !(size & 2)) return unimpl_simd(c, i);
        int dbl = size & 1, fes = dbl ? 8 : 4;
        for (int e = 0; e < dsz / fes; e++) { uint64_t v = lane_get(A, fes, e); lane_set(res, fes, e, dbl ? pk_d(sqrt(fp_d(v, 1))) : pk_f(sqrtf(fp_f(v)))); }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x1D: {                                                 /* SCVTF / UCVTF */
        int dbl = size & 1, fes = dbl ? 8 : 4;
        int cnt = scalar ? 1 : dsz / fes;
        for (int e = 0; e < cnt; e++) {
            uint64_t v = lane_get(A, fes, e);
            double d = U ? (double)v : (double)sxl(v, fes); float f = U ? (float)v : (float)sxl(v, fes);
            lane_set(res, fes, e, dbl ? pk_d(d) : pk_f(f));
        }
        if (scalar) { c->vr[rd][0] = lane_get(res, fes, 0); c->vr[rd][1] = 0; return 0; }
        vstore_res(c, rd, res, Q); return 0;
    }
    case 0x1B: case 0x1A: case 0x1C: {                           /* FCVTZS/FCVTZU (0x1B, size<1>=1); FCVTMS/NS etc. (0x1A,0x1B size<1>=0) */
        int dbl = size & 1, fes = dbl ? 8 : 4, mode;
        if (op == 0x1B && (size & 2)) mode = 3;
        else if (op == 0x1B) mode = 2;                           /* FCVTMS/FCVTMU */
        else if (op == 0x1A) mode = (size & 2) ? 1 : 0;          /* FCVTPS vs FCVTNS */
        else mode = (size & 2) ? -1 : 4;                         /* FCVTAS */
        if (mode < 0) return unimpl_simd(c, i);
        int cnt = scalar ? 1 : dsz / fes;
        for (int e = 0; e < cnt; e++) lane_set(res, fes, e, fp_to_int(fp_d(lane_get(A, fes, e), dbl), fes * 8, !U, mode));
        if (scalar) { c->vr[rd][0] = lane_get(res, fes, 0); c->vr[rd][1] = 0; return 0; }
        vstore_res(c, rd, res, Q); return 0;
    }
    default: return unimpl_simd(c, i);
    }
}

static int simd_across(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, U = (i >> 29) & 1, size = (i >> 22) & 3, op = (i >> 12) & 31, rn = (i >> 5) & 31, rd = i & 31;
    int es = 1 << size, n = (Q ? 16 : 8) / es;
    if (size == 3) return unimpl_simd(c, i);
    uint8_t A[16]; memcpy(A, VB(c, rn), 16);
    uint64_t r = 0;
    if (op == 0x1B && !U) { for (int e = 0; e < n; e++) r += lane_get(A, es, e); r &= lmask(es); }
    else if (op == 0x03) {                                       /* SADDLV / UADDLV */
        int64_t s = 0; for (int e = 0; e < n; e++) { uint64_t v = lane_get(A, es, e); s += U ? (int64_t)v : sxl(v, es); }
        es = es * 2; r = (uint64_t)s & lmask(es); c->vr[rd][0] = r; c->vr[rd][1] = 0; return 0;
    }
    else if (op == 0x0A || op == 0x1A) {
        bool mx = op == 0x0A; int64_t best = 0; uint64_t ub = 0;
        for (int e = 0; e < n; e++) {
            uint64_t v = lane_get(A, es, e); int64_t sv = sxl(v, es);
            if (!e) { best = sv; ub = v; }
            else if (U) { if (mx ? v > ub : v < ub) ub = v; } else { if (mx ? sv > best : sv < best) best = sv; }
        }
        r = (U ? ub : (uint64_t)best) & lmask(es);
    } else return unimpl_simd(c, i);
    c->vr[rd][0] = r; c->vr[rd][1] = 0; return 0;
}

static int simd_shift_imm(A64 *c, uint32_t i, bool scalar) {
    int Q = scalar ? 1 : (i >> 30) & 1, U = (i >> 29) & 1, immh = (i >> 19) & 15, immb = (i >> 16) & 7, op = (i >> 11) & 31;
    int rn = (i >> 5) & 31, rd = i & 31;
    if (!immh) return unimpl_simd(c, i);
    int sz = immh & 8 ? 3 : immh & 4 ? 2 : immh & 2 ? 1 : 0, es = 1 << sz, bits = es * 8, sh_imm = (immh << 3) | immb;
    uint8_t A[16], D[16], res[16] = {0}; memcpy(A, VB(c, rn), 16); memcpy(D, VB(c, rd), 16);
    int dsz = scalar ? 8 : (Q ? 16 : 8), n = scalar ? 1 : dsz / es;
    if (scalar && sz != 3) return unimpl_simd(c, i);
    int rsh = 2 * bits - sh_imm, lsh = sh_imm - bits;
    switch (op) {
    case 0x00: case 0x02: {                                      /* SSHR/USHR, SSRA/USRA */
        for (int e = 0; e < n; e++) {
            uint64_t a = lane_get(A, es, e), r = U ? (rsh >= 64 ? 0 : a >> rsh) : asr_lane(a, es, rsh >= bits ? bits - 1 : rsh);
            if (op == 0x02) r += lane_get(D, es, e);
            lane_set(res, es, e, r & lmask(es));
        }
        break;
    }
    case 0x0A: {                                                 /* SHL / SLI */
        for (int e = 0; e < n; e++) {
            uint64_t a = lane_get(A, es, e), r = (a << lsh) & lmask(es);
            if (U) { uint64_t keep = lsh ? (lmask(es) >> (bits - lsh)) : 0; r |= lane_get(D, es, e) & keep; }
            lane_set(res, es, e, r);
        }
        break;
    }
    case 0x08: {                                                 /* SRI */
        if (!U) return unimpl_simd(c, i);
        for (int e = 0; e < n; e++) {
            uint64_t a = lane_get(A, es, e), r = rsh >= 64 ? 0 : a >> rsh, keep = rsh >= bits ? 0 : (lmask(es) << (bits - rsh)) & lmask(es);
            lane_set(res, es, e, r | (lane_get(D, es, e) & keep));
        }
        break;
    }
    case 0x14: {                                                 /* SSHLL / USHLL (SXTL / UXTL) */
        if (sz == 3) return unimpl_simd(c, i);
        int ne = 8 / es, des = es * 2;
        for (int e = 0; e < ne; e++) {
            uint64_t a = lane_get(A + (Q ? 8 : 0), es, e); uint64_t v = U ? a : (uint64_t)sxl(a, es);
            lane_set(res, des, e, (v << lsh) & lmask(des));
        }
        memcpy(VB(c, rd), res, 16); return 0;
    }
    case 0x10: {                                                 /* SHRN / SHRN2 */
        if (U || sz == 3) return unimpl_simd(c, i);
        int ses = es * 2, ne = 8 / es; uint8_t tmp[16] = {0};
        int rs = 2 * bits - sh_imm;
        for (int e = 0; e < ne; e++) lane_set(tmp, es, e, (lane_get(A, ses, e) >> rs) & lmask(es));
        uint8_t *d = VB(c, rd);
        if (Q) memcpy(d + 8, tmp, 8); else { memcpy(d, tmp, 8); memset(d + 8, 0, 8); }
        return 0;
    }
    default: return unimpl_simd(c, i);
    }
    if (scalar) { c->vr[rd][0] = lane_get(res, 8, 0); c->vr[rd][1] = 0; return 0; }
    vstore_res(c, rd, res, Q); return 0;
}

static int simd_permute(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, size = (i >> 22) & 3, rm = (i >> 16) & 31, op = (i >> 12) & 7, rn = (i >> 5) & 31, rd = i & 31;
    int es = 1 << size, dsz = Q ? 16 : 8, n = dsz / es;
    if (size == 3 && !Q) return unimpl_simd(c, i);
    uint8_t A[16], B[16], res[16] = {0}; memcpy(A, VB(c, rn), 16); memcpy(B, VB(c, rm), 16);
    int part = op >> 2;
    for (int e = 0; e < n; e++) {
        uint64_t v;
        switch (op & 3) {
        case 1: { int k = 2 * e + part; v = k < n ? lane_get(A, es, k) : lane_get(B, es, k - n); break; }       /* UZP */
        case 2: { int k = (e & ~1) + part; v = (e & 1) ? lane_get(B, es, k) : lane_get(A, es, k); break; }      /* TRN */
        case 3: { int k = (e >> 1) + part * (n / 2); v = (e & 1) ? lane_get(B, es, k) : lane_get(A, es, k); break; }   /* ZIP */
        default: return unimpl_simd(c, i);
        }
        lane_set(res, es, e, v);
    }
    vstore_res(c, rd, res, Q); return 0;
}

static int simd_ext_tbl(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
    uint8_t res[16] = {0};
    if ((i >> 29) & 1) {                                         /* EXT */
        int imm4 = (i >> 11) & 15; if (!Q && imm4 > 7) return unimpl_simd(c, i);
        uint8_t cat[32]; memcpy(cat, VB(c, rn), 16); memcpy(cat + 16, VB(c, rm), 16);
        if (!Q) { memcpy(cat, VB(c, rn), 8); memcpy(cat + 8, VB(c, rm), 8); }
        memcpy(res, cat + imm4, Q ? 16 : 8);
    } else {                                                     /* TBL / TBX */
        int len = ((i >> 13) & 3) + 1, tbx = (i >> 12) & 1; uint8_t tab[64], idx[16], d[16];
        for (int k = 0; k < len; k++) memcpy(tab + 16 * k, VB(c, (rn + k) & 31), 16);
        memcpy(idx, VB(c, rm), 16); memcpy(d, VB(c, rd), 16);
        for (int b = 0; b < (Q ? 16 : 8); b++) res[b] = idx[b] < 16 * len ? tab[idx[b]] : (tbx ? d[b] : 0);
    }
    vstore_res(c, rd, res, Q); return 0;
}

static int simd_scalar_3same(A64 *c, uint32_t i) {
    int U = (i >> 29) & 1, size = (i >> 22) & 3, op = (i >> 11) & 31, rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
    if (size != 3 || op >= 0x18) return unimpl_simd(c, i);
    uint64_t a = c->vr[rn][0], b = c->vr[rm][0], r; int64_t sa = (int64_t)a, sb = (int64_t)b;
    switch (op) {
    case 0x06: r = (U ? a > b : sa > sb) ? ~0ull : 0; break;
    case 0x07: r = (U ? a >= b : sa >= sb) ? ~0ull : 0; break;
    case 0x08: { int sh = (int8_t)(b & 0xFF); if (sh >= 0) r = sh >= 64 ? 0 : a << sh; else { int rs = -sh; r = U ? (rs >= 64 ? 0 : a >> rs) : (uint64_t)(sa >> (rs >= 64 ? 63 : rs)); } break; }
    case 0x10: r = U ? a - b : a + b; break;
    case 0x11: r = U ? (a == b ? ~0ull : 0) : ((a & b) ? ~0ull : 0); break;
    default: return unimpl_simd(c, i);
    }
    c->vr[rd][0] = r; c->vr[rd][1] = 0; return 0;
}

/* ---------------------------------------------------------------- dispatch for the data-processing groups */

/* ---------------------------------------------------------------- Armv8 crypto extension: SHA-1, SHA-256, AES (0.4.26: the iOS kernel's corecrypto uses them right after boot)
 * Implemented from the Arm ARM pseudocode (SHA1C/P/M, SHA1H, SHA1SU0/1, SHA256H/H2/SU0/SU1, AESE/AESD/AESMC/AESIMC). No keys or secrets are involved:
 * these are public hash/cipher primitives. Not implemented: PMULL, SHA-512, SHA-3, SM3/SM4. */
static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static uint8_t g_sbox[256], g_isbox[256]; static int g_sbox_ok;
static uint8_t gmul8(uint8_t a, uint8_t b) { uint8_t p = 0; while (b) { if (b & 1) p ^= a; a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); b >>= 1; } return p; }
static void sbox_init(void) {
    if (g_sbox_ok) return;
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0; if (x) for (int y = 1; y < 256; y++) if (gmul8((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t b = inv, r = b;
        for (int k = 1; k <= 4; k++) { b = (uint8_t)((b << 1) | (b >> 7)); r ^= b; }
        r ^= 0x63; g_sbox[x] = r; g_isbox[r] = (uint8_t)x;
    }
    g_sbox_ok = 1;
}
static void aes_mix(uint8_t *s, bool inv) {
    for (int col = 0; col < 4; col++) {
        uint8_t a0 = s[4*col], a1 = s[4*col+1], a2 = s[4*col+2], a3 = s[4*col+3];
        if (!inv) {
            s[4*col]   = (uint8_t)(gmul8(a0,2) ^ gmul8(a1,3) ^ a2 ^ a3);
            s[4*col+1] = (uint8_t)(a0 ^ gmul8(a1,2) ^ gmul8(a2,3) ^ a3);
            s[4*col+2] = (uint8_t)(a0 ^ a1 ^ gmul8(a2,2) ^ gmul8(a3,3));
            s[4*col+3] = (uint8_t)(gmul8(a0,3) ^ a1 ^ a2 ^ gmul8(a3,2));
        } else {
            s[4*col]   = (uint8_t)(gmul8(a0,14) ^ gmul8(a1,11) ^ gmul8(a2,13) ^ gmul8(a3,9));
            s[4*col+1] = (uint8_t)(gmul8(a0,9) ^ gmul8(a1,14) ^ gmul8(a2,11) ^ gmul8(a3,13));
            s[4*col+2] = (uint8_t)(gmul8(a0,13) ^ gmul8(a1,9) ^ gmul8(a2,14) ^ gmul8(a3,11));
            s[4*col+3] = (uint8_t)(gmul8(a0,11) ^ gmul8(a1,13) ^ gmul8(a2,9) ^ gmul8(a3,14));
        }
    }
}
static uint32_t sha_ch(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static uint32_t sha_par(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t sha_maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }
static void sha256_hash(uint32_t *X, uint32_t *Y, const uint32_t *W, bool part1, uint32_t *out) {
    uint32_t x[4], y[4]; memcpy(x, X, 16); memcpy(y, Y, 16);
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_ch(y[0], y[1], y[2]), maj = sha_maj(x[0], x[1], x[2]);
        uint32_t t = y[3] + (ror32(y[0], 6) ^ ror32(y[0], 11) ^ ror32(y[0], 25)) + chs + W[e];
        x[3] = t + x[3];
        y[3] = t + (ror32(x[0], 2) ^ ror32(x[0], 13) ^ ror32(x[0], 22)) + maj;
        uint32_t nx[4] = { y[3], x[0], x[1], x[2] }, ny[4] = { x[3], y[0], y[1], y[2] };   /* <Y,X> = ROL(Y:X, 32) */
        memcpy(x, nx, 16); memcpy(y, ny, 16);
    }
    memcpy(out, part1 ? x : y, 16);
}
#define NOT_CRYPTO 0x7FFF
static void clmul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t l = 0, h = 0;
    for (unsigned k = 0; k < 64; k++) if ((b >> k) & 1) { l ^= a << k; if (k) h ^= a >> (64 - k); }
    *lo = l; *hi = h;
}
static int simd_crypto(A64 *c, uint32_t i) {
    int rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
    if ((i & 0xBF20FC00u) == 0x0E20E000u) {                                   /* PMULL / PMULL2: polynomial (carry-less) multiply long */
        int size = (i >> 22) & 3, q = (i >> 30) & 1; if (size != 0 && size != 3) return NOT_CRYPTO;
        if (size == 3) {                                                        /* 64 x 64 -> 128 (the "PMULL" crypto feature, used by GCM/CRC code) */
            uint64_t a = c->vr[rn][q], b = c->vr[rm][q], lo, hi; clmul64(a, b, &lo, &hi); c->vr[rd][0] = lo; c->vr[rd][1] = hi; return 0;
        }
        const uint8_t *a = VB(c, rn) + 8 * q, *b = VB(c, rm) + 8 * q; uint16_t r[8];
        for (int e = 0; e < 8; e++) { uint16_t x = 0; for (int k = 0; k < 8; k++) if ((b[e] >> k) & 1) x ^= (uint16_t)((uint16_t)a[e] << k); r[e] = x; }
        memcpy(VB(c, rd), r, 16); return 0;                                     /* 8 x 8 -> 16, per byte lane */
    }
    if ((i & 0xFFFF0C00u) == 0x4E280800u) {                                   /* AES: AESE AESD AESMC AESIMC */
        unsigned op = (i >> 12) & 31; if (op < 4 || op > 7) return NOT_CRYPTO;
        sbox_init(); uint8_t st[16], r[16]; memcpy(st, VB(c, rn), 16);
        if (op <= 5) {
            uint8_t d[16]; memcpy(d, VB(c, rd), 16); for (int k = 0; k < 16; k++) st[k] = (uint8_t)(d[k] ^ st[k]);
            static const uint8_t sr[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11}, isr[16] = {0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3};
            for (int k = 0; k < 16; k++) r[k] = (op == 4) ? g_sbox[st[sr[k]]] : g_isbox[st[isr[k]]];
        } else { memcpy(r, st, 16); aes_mix(r, op == 7); }
        memcpy(VB(c, rd), r, 16); return 0;
    }
    if ((i & 0xFFFF0C00u) == 0x5E280800u) {                                   /* SHA1H, SHA1SU1, SHA256SU0 */
        unsigned op = (i >> 12) & 31; uint32_t d[4], n[4]; memcpy(d, VB(c, rd), 16); memcpy(n, VB(c, rn), 16);
        if (op == 0) { c->vr[rd][0] = rol32(n[0], 30); c->vr[rd][1] = 0; return 0; }
        if (op == 1) { uint32_t t[4] = { d[0] ^ n[1], d[1] ^ n[2], d[2] ^ n[3], d[3] }, r[4];
            r[0] = rol32(t[0], 1); r[1] = rol32(t[1], 1); r[2] = rol32(t[2], 1); r[3] = rol32(t[3], 1) ^ rol32(t[0], 2); memcpy(VB(c, rd), r, 16); return 0; }
        if (op == 2) { uint32_t t[4] = { d[1], d[2], d[3], n[0] }, r[4];
            for (int e = 0; e < 4; e++) r[e] = (ror32(t[e], 7) ^ ror32(t[e], 18) ^ (t[e] >> 3)) + d[e]; memcpy(VB(c, rd), r, 16); return 0; }
        return NOT_CRYPTO;
    }
    if ((i & 0xFF208C00u) == 0x5E000000u) {                                   /* SHA1C SHA1P SHA1M SHA1SU0 SHA256H SHA256H2 SHA256SU1 */
        unsigned op = (i >> 12) & 7; uint32_t d[4], n[4], m[4]; memcpy(d, VB(c, rd), 16); memcpy(n, VB(c, rn), 16); memcpy(m, VB(c, rm), 16);
        if (op <= 2) {
            uint32_t X[4]; memcpy(X, d, 16); uint32_t Y = n[0];
            for (int e = 0; e < 4; e++) {
                uint32_t t = op == 0 ? sha_ch(X[1], X[2], X[3]) : op == 1 ? sha_par(X[1], X[2], X[3]) : sha_maj(X[1], X[2], X[3]);
                Y = Y + rol32(X[0], 5) + t + m[e]; X[1] = rol32(X[1], 30);
                uint32_t nx[4] = { Y, X[0], X[1], X[2] }; Y = X[3]; memcpy(X, nx, 16);   /* <Y,X> = ROL(Y:X, 32) */
            }
            memcpy(VB(c, rd), X, 16); return 0;
        }
        if (op == 3) { uint32_t r[4];
            /* op2 = Vn<63:0> : Vd<127:64>, result = Vd ^ op2 ^ Vm */
            uint32_t o2[4] = { d[2], d[3], n[0], n[1] };
            for (int e = 0; e < 4; e++) r[e] = d[e] ^ o2[e] ^ m[e];
            memcpy(VB(c, rd), r, 16); return 0; }
        if (op == 4 || op == 5) { uint32_t r[4]; if (op == 4) sha256_hash(d, n, m, true, r); else sha256_hash(n, d, m, false, r); memcpy(VB(c, rd), r, 16); return 0; }
        if (op == 6) {
            uint32_t t0[4] = { n[1], n[2], n[3], m[0] }, w[4];   /* T0 = Vm<31:0> : Vn<127:32> */
            for (int e = 0; e < 4; e++) {
                uint32_t elt = e < 2 ? m[2 + e] : w[e - 2];
                elt = ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10); w[e] = elt + d[e] + t0[e];
            }
            memcpy(VB(c, rd), w, 16); return 0;
        }
    }
    return NOT_CRYPTO;
}
/* ---------------------------------------------------------------- AdvSIMD "three registers of different length" (0.4.30): SADDL/UADDL SADDW/UADDW SSUBL/USUBL SSUBW/USUBW
 * ADDHN/RADDHN SUBHN/RSUBHN SABAL/UABAL SABDL/UABDL SMLAL/UMLAL SMLSL/UMLSL SMULL/UMULL (+ the "2" forms on the upper halves). Not: SQDMLAL/SQDMLSL/SQDMULL, PMULL (crypto, above). */
static uint64_t ext_el(uint64_t v, int es, bool sg) { return sg ? (uint64_t)sxl(v, es) : (v & lmask(es)); }
static int simd_3diff(A64 *c, uint32_t i) {
    int rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31, size = (i >> 22) & 3, op = (i >> 12) & 15, q = (i >> 30) & 1; bool u = (i >> 29) & 1, sg = !u;
    if (size == 3 || op == 9 || op == 11 || op == 13 || op == 14 || op == 15) return unimpl_simd(c, i);
    int en = 1 << size, ew = en * 2, lanes = 8 >> size; uint8_t n[16], m[16], d[16], r[16];
    memcpy(n, VB(c, rn), 16); memcpy(m, VB(c, rm), 16); memcpy(d, VB(c, rd), 16); memset(r, 0, 16);
    if (op == 4 || op == 6) {                                                   /* ADDHN/RADDHN/SUBHN/RSUBHN: wide sources, narrow result (high half) */
        int ewide = ew, enar = en; uint64_t rnd = u ? (1ull << (enar * 8 - 1)) : 0;
        if (q) memcpy(r, d, 8);
        for (int e = 0; e < 8 / enar * 1 && e < lanes; e++) {
            uint64_t a = lane_get(n, ewide, e), b = lane_get(m, ewide, e);
            uint64_t t = (op == 4 ? a + b : a - b) + rnd; t &= lmask(ewide); lane_set(r + (q ? 8 : 0), enar, e, (t >> (enar * 8)) & lmask(enar));
        }
        memcpy(VB(c, rd), r, 16); return 0;
    }
    for (int e = 0; e < lanes; e++) {
        int idx = e + (q ? lanes : 0);
        uint64_t a = (op == 1 || op == 3) ? lane_get(n, ew, e) : ext_el(lane_get(n, en, idx), en, sg);
        uint64_t b = ext_el(lane_get(m, en, idx), en, sg), x = lane_get(d, ew, e), res;
        switch (op) {
        case 0: res = a + b; break;
        case 1: res = a + b; break;
        case 2: res = a - b; break;
        case 3: res = a - b; break;
        case 5: case 7: { int64_t sa = (int64_t)a, sb = (int64_t)b; uint64_t df = sg ? (uint64_t)(sa > sb ? sa - sb : sb - sa) : (a > b ? a - b : b - a); res = (op == 5) ? x + df : df; break; }
        case 8: res = x + a * b; break;
        case 10: res = x - a * b; break;
        default: res = a * b; break;                                            /* 12: SMULL/UMULL */
        }
        lane_set(r, ew, e, res & lmask(ew));
    }
    memcpy(VB(c, rd), r, 16); return 0;
}

/* ---------------------------------------------------------------- AdvSIMD "vector x indexed element" (0.4.30): MUL MLA MLS, SMULL/UMULL SMLAL/UMLAL SMLSL/UMLSL (+2), FMUL FMLA FMLS (single, double) */
static int simd_indexed(A64 *c, uint32_t i) {
    int rd = i & 31, rn = (i >> 5) & 31, q = (i >> 30) & 1, size = (i >> 22) & 3, op = (i >> 12) & 15; bool u = (i >> 29) & 1;
    int l = (i >> 21) & 1, mbit = (i >> 20) & 1, h = (i >> 11) & 1, rmlo = (i >> 16) & 15;
    uint8_t n[16], m[16], d[16], r[16]; memcpy(n, VB(c, rn), 16); memcpy(d, VB(c, rd), 16); memset(r, 0, 16);
    if (size >= 2 && (op == 1 || op == 5 || op == 9) && !u) {                   /* FMLA / FMLS / FMUL by element */
        int dbl = size & 1, rm = (mbit << 4) | rmlo, idx = dbl ? h : ((h << 1) | l); if (dbl && l) return unimpl_simd(c, i);
        memcpy(m, VB(c, rm), 16); int es = dbl ? 8 : 4, lanes = (q ? 16 : 8) / es; uint64_t sc = lane_get(m, es, idx);
        for (int e = 0; e < lanes; e++) {
            uint64_t nb = lane_get(n, es, e), db = lane_get(d, es, e), res;
            if (op == 9) res = fp_bin(F_MUL, nb, sc, dbl);
            else { double x = fp_d(nb, dbl), y = fp_d(sc, dbl), a = fp_d(db, dbl); if (op == 5) x = -x;
                   res = dbl ? pk_d(fma(x, y, a)) : pk_f(fmaf((float)x, (float)y, (float)a)); }
            lane_set(r, es, e, res);
        }
        memcpy(VB(c, rd), r, 16); return 0;
    }
    if (size == 0 || size == 3) return unimpl_simd(c, i);
    int rm, idx; if (size == 1) { rm = rmlo; idx = (h << 2) | (l << 1) | mbit; } else { rm = (mbit << 4) | rmlo; idx = (h << 1) | l; }
    memcpy(m, VB(c, rm), 16); int es = 1 << size; uint64_t sc = lane_get(m, es, idx);
    if (op == 8 || (u && (op == 0 || op == 4))) {                               /* MUL, MLA, MLS (same-size) */
        int lanes = (q ? 16 : 8) / es;
        for (int e = 0; e < lanes; e++) {
            uint64_t p = (lane_get(n, es, e) * sc) & lmask(es), db = lane_get(d, es, e);
            lane_set(r, es, e, (op == 8 ? p : op == 0 ? db + p : db - p) & lmask(es));
        }
        memcpy(VB(c, rd), r, 16); return 0;
    }
    if (op == 2 || op == 6 || op == 10) {                                       /* SMLAL/UMLAL, SMLSL/UMLSL, SMULL/UMULL (+2) */
        int ew = es * 2, lanes = 8 / es; bool sg = !u; uint64_t b = ext_el(sc, es, sg);
        for (int e = 0; e < lanes; e++) {
            uint64_t a = ext_el(lane_get(n, es, e + (q ? lanes : 0)), es, sg), x = lane_get(d, ew, e), p = a * b;
            lane_set(r, ew, e, (op == 2 ? x + p : op == 6 ? x - p : p) & lmask(ew));
        }
        memcpy(VB(c, rd), r, 16); return 0;
    }
    return unimpl_simd(c, i);
}

/* 0.4.51: scalar FMLA / FMLS / FMUL (by element) on S or D registers: Vd = Vn (op) Vm.<T>[idx]; the rest of Vd is zeroed */
static int simd_indexed_scalar(A64 *c, uint32_t i) {
    int rd = i & 31, rn = (i >> 5) & 31, size = (i >> 22) & 3, op = (i >> 12) & 15; bool u = (i >> 29) & 1;
    int l = (i >> 21) & 1, mbit = (i >> 20) & 1, h = (i >> 11) & 1, rmlo = (i >> 16) & 15;
    if (u || size < 2 || !(op == 1 || op == 5 || op == 9)) return unimpl_simd(c, i);
    int dbl = size & 1, rm = (mbit << 4) | rmlo, idx = dbl ? h : ((h << 1) | l); if (dbl && l) return unimpl_simd(c, i);
    int es = dbl ? 8 : 4; uint64_t sc = lane_get(VB(c, rm), es, idx), nb = vread_scalar(c, rn, es), db = vread_scalar(c, rd, es), res;
    if (op == 9) res = fp_bin(F_MUL, nb, sc, dbl);
    else { double x = fp_d(nb, dbl), y = fp_d(sc, dbl), a = fp_d(db, dbl); if (op == 5) x = -x;
           res = dbl ? pk_d(fma(x, y, a)) : pk_f(fmaf((float)x, (float)y, (float)a)); }
    vwrite_scalar(c, rd, es, res); return 0;
}

static int simd_dp(A64 *c, uint32_t i) {
    { int cr = simd_crypto(c, i); if (cr != NOT_CRYPTO) return cr; }
    unsigned g = (i >> 24) & 0x1F;
    int rn = (i >> 5) & 31, rd = i & 31;
    if (g == 0x1E || g == 0x1F) {
        if (!((i >> 30) & 1)) { if (!((i >> 29) & 1)) return simd_fp_scalar(c, i); return unimpl_simd(c, i); }
        if ((i >> 31) & 1) return unimpl_simd(c, i);
        if (g == 0x1F) { if (!((i >> 10) & 1)) return simd_indexed_scalar(c, i); if ((i >> 23) & 15) return simd_shift_imm(c, i, true); return unimpl_simd(c, i); }
        /* scalar AdvSIMD, bits 28:24 = 11110 */
        if (!((i >> 21) & 1)) {
            if (((i >> 29) & 1) == 0 && ((i >> 22) & 3) == 0 && ((i >> 11) & 15) == 0 && ((i >> 10) & 1) == 1) {   /* DUP (element), scalar */
                int imm5 = (i >> 16) & 31; if (!(imm5 & 15)) return unimpl_simd(c, i);
                int sz = __builtin_ctz((unsigned)imm5), es = 1 << sz, idx = imm5 >> (sz + 1);
                if (sz > 3) return unimpl_simd(c, i);
                c->vr[rd][0] = lane_get(VB(c, rn), es, idx) & lmask(es); c->vr[rd][1] = 0; return 0;
            }
            return unimpl_simd(c, i);
        }
        if ((i >> 10) & 1) return simd_scalar_3same(c, i);
        if (((i >> 10) & 3) == 2) {
            if (((i >> 17) & 0x1F) == 0x10) return simd_2misc(c, i, true);
            if (((i >> 17) & 0x1F) == 0x18) {                                          /* scalar pairwise ADDP / FADDP */
                int size = (i >> 22) & 3, op = (i >> 12) & 31; uint64_t *s = c->vr[rn];
                if (op == 0x1B && !((i >> 29) & 1) && size == 3) { c->vr[rd][0] = s[0] + s[1]; c->vr[rd][1] = 0; return 0; }
                if (op == 0x0D && ((i >> 29) & 1) && !(size & 2)) {
                    if (size & 1) vwrite_scalar(c, rd, 8, fp_bin(F_ADD, s[0], s[1], 1));
                    else vwrite_scalar(c, rd, 4, fp_bin(F_ADD, s[0] & 0xFFFFFFFFull, s[0] >> 32, 0));
                    return 0;
                }
            }
        }
        return unimpl_simd(c, i);
    }
    if (g == 0x0F && !((i >> 31) & 1)) {
        if (!((i >> 10) & 1)) return simd_indexed(c, i);                                 /* vector x indexed element */
        if (((i >> 19) & 15) == 0) return simd_modimm(c, i);
        return simd_shift_imm(c, i, false);
    }
    if (g == 0x0E && !((i >> 31) & 1)) {
        if ((i >> 21) & 1) {
            if ((i >> 10) & 1) return simd_3same(c, i);
            if (((i >> 10) & 3) == 2) {
                if (((i >> 17) & 0x1F) == 0x10) return simd_2misc(c, i, false);
                if (((i >> 17) & 0x1F) == 0x18) return simd_across(c, i);
            }
            if (((i >> 10) & 3) == 0) return simd_3diff(c, i);
            return unimpl_simd(c, i);
        }
        if (((i >> 15) & 1)) return unimpl_simd(c, i);
        if ((i >> 10) & 1) { if (((i >> 22) & 3) == 0) return simd_copy(c, i); return unimpl_simd(c, i); }
        if ((i >> 29) & 1) { if (((i >> 22) & 3) == 0) return simd_ext_tbl(c, i); return unimpl_simd(c, i); }
        if (((i >> 10) & 3) == 0) { if (((i >> 22) & 3) == 0) return simd_ext_tbl(c, i); return unimpl_simd(c, i); }
        if (((i >> 10) & 3) == 2) return simd_permute(c, i);
    }
    return unimpl_simd(c, i);
}

/* ---------------------------------------------------------------- loads and stores */
static int vld(A64 *c, uint64_t a, int bytes, int r) {
    uint64_t lo = 0, hi = 0;
    if (bytes == 16) { TRY(ld(c, a, 3, &lo)); TRY(ld(c, a + 8, 3, &hi)); }
    else { int sz = bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0; TRY(ld(c, a, sz, &lo)); }
    c->vr[r][0] = lo; c->vr[r][1] = hi; return 0;
}
static int vst(A64 *c, uint64_t a, int bytes, int r) {
    if (bytes == 16) { TRY(st(c, a, 3, c->vr[r][0])); TRY(st(c, a + 8, 3, c->vr[r][1])); }
    else { int sz = bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0; TRY(st(c, a, sz, c->vr[r][0] & lmask(bytes))); }
    return 0;
}

static int simd_structure(A64 *c, uint32_t i) {
    int Q = (i >> 30) & 1, L = (i >> 22) & 1, rn = (i >> 5) & 31, rt = i & 31;
    bool post = (i >> 23) & 1, single = (i >> 24) & 1;
    int rm = (i >> 16) & 31;
    uint64_t base = R(c, rn, true), addr = base; int total;
    if (!single) {
        int opc = (i >> 12) & 15, size = (i >> 10) & 3, rpt, selem;
        switch (opc) { case 0: rpt = 1; selem = 4; break; case 2: rpt = 4; selem = 1; break; case 4: rpt = 1; selem = 3; break; case 6: rpt = 3; selem = 1; break;
                       case 7: rpt = 1; selem = 1; break; case 8: rpt = 1; selem = 2; break; case 10: rpt = 2; selem = 1; break; default: return unimpl_simd(c, i); }
        if (size == 3 && !Q && selem != 1) return unimpl_simd(c, i);
        int es = 1 << size, dsz = Q ? 16 : 8, ne = dsz / es;
        total = rpt * selem * dsz;
        for (int r = 0; r < rpt; r++) for (int e = 0; e < ne; e++) for (int s = 0; s < selem; s++) {
            int tt = (rt + r + s) & 31; uint64_t v;
            if (L) { TRY(ld(c, addr, size, &v)); lane_set(VB(c, tt), es, e, v); }
            else TRY(st(c, addr, size, lane_get(VB(c, tt), es, e)));
            addr += (uint64_t)es;
        }
        if (L && !Q) for (int r = 0; r < rpt; r++) for (int s = 0; s < selem; s++) c->vr[(rt + r + s) & 31][1] = 0;
    } else {
        int opcode = (i >> 13) & 7, S = (i >> 12) & 1, size = (i >> 10) & 3, R_ = (i >> 21) & 1;
        int scale = opcode >> 1, selem = ((opcode & 1) << 1 | R_) + 1, index = 0; bool rep = false;
        switch (scale) {
        case 0: index = (Q << 3) | (S << 2) | size; break;
        case 1: if (size & 1) return unimpl_simd(c, i); index = (Q << 2) | (S << 1) | (size >> 1); break;
        case 2: if (!(size & 2)) { index = (Q << 1) | S; } else { if (S) return unimpl_simd(c, i); index = Q; scale = 3; } break;
        default: if (L == 0 || S) return unimpl_simd(c, i); rep = true; scale = size; break;
        }
        int es = 1 << scale; total = selem * es;
        for (int s = 0; s < selem; s++) {
            int tt = (rt + s) & 31; uint64_t v;
            if (rep) { TRY(ld(c, addr, scale, &v)); uint8_t res[16] = {0}; for (int e = 0; e < (Q ? 16 : 8) / es; e++) lane_set(res, es, e, v); vstore_res(c, tt, res, Q); }
            else if (L) { TRY(ld(c, addr, scale, &v)); lane_set(VB(c, tt), es, index, v); }
            else TRY(st(c, addr, scale, lane_get(VB(c, tt), es, index)));
            addr += (uint64_t)es;
        }
    }
    if (post) W(c, rn, base + (rm == 31 ? (uint64_t)total : R(c, rm, false)), true);
    return 0;
}

static int simd_ldst(A64 *c, uint32_t i, uint64_t *next) {
    (void)next;
    int rt = i & 31, rn = (i >> 5) & 31;
    unsigned g = (i >> 24) & 0x3F;
    if (g == 0x0C || g == 0x0D) {                                /* structure loads/stores (multiple / single, with or without post-index) */
        if ((i >> 31) & 1) return unimpl_simd(c, i);
        if (g == 0x0C && !((i >> 23) & 1) && (i & 0x1F0000u)) return unimpl_simd(c, i);   /* no-offset multiple form needs Rm field = 0 */
        return simd_structure(c, i);
    }
    if (g == 0x1C) {                                             /* LDR (literal) */
        int opc = i >> 30; if (opc == 3) return unimpl_simd(c, i);
        return vld(c, c->pc + (uint64_t)(sx((i >> 5) & 0x7FFFF, 19) * 4), 4 << opc, rt);
    }
    if (((i >> 27) & 7) == 5) {                                  /* LDP / STP */
        int opc = i >> 30, type = (i >> 23) & 3, rt2 = (i >> 10) & 31; bool L = (i >> 22) & 1;
        if (opc == 3) return unimpl_simd(c, i);
        int by = 4 << opc;
        int64_t off = sx((i >> 15) & 0x7F, 7) * by;
        uint64_t base = R(c, rn, true), addr = type == 1 ? base : base + (uint64_t)off;
        if (L) { TRY(vld(c, addr, by, rt)); TRY(vld(c, addr + (uint64_t)by, by, rt2)); }
        else { TRY(vst(c, addr, by, rt)); TRY(vst(c, addr + (uint64_t)by, by, rt2)); }
        if (type == 1) W(c, rn, base + (uint64_t)off, true); else if (type == 3) W(c, rn, addr, true);
        return 0;
    }
    if (((i >> 27) & 7) == 7) {                                  /* LDR / STR (immediate, register) */
        int size = i >> 30, opc = (i >> 22) & 3;
        int by = (opc & 2) ? 16 : (1 << size);
        if ((opc & 2) && size != 0) return unimpl_simd(c, i);
        int shift = (opc & 2) ? 4 : size;
        bool load = opc & 1;
        uint64_t base = R(c, rn, true), addr; int wb = 0;
        if ((i >> 24) & 1) addr = base + (((uint64_t)(i >> 10) & 0xFFF) << shift);
        else if ((i >> 21) & 1) {
            if (((i >> 10) & 3) != 2) return unimpl_simd(c, i);
            int opt = (i >> 13) & 7, S = (i >> 12) & 1, rm = (i >> 16) & 31;
            if (!(opt & 2)) return unimpl_simd(c, i);
            addr = base + extend_reg(R(c, rm, false), opt, S ? shift : 0);
        } else {
            int64_t imm = sx((i >> 12) & 0x1FF, 9); int t = (i >> 10) & 3;
            addr = t == 1 ? base : base + (uint64_t)imm; wb = t == 1 ? 1 : t == 3 ? 2 : 0;
            if (wb) base = base + (uint64_t)imm;
        }
        if (load) TRY(vld(c, addr, by, rt)); else TRY(vst(c, addr, by, rt));
        if (wb) W(c, rn, base, true);
        return 0;
    }
    return unimpl_simd(c, i);
}
