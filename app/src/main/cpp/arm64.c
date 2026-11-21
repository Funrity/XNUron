/* XNUron ARM64 core - see arm64.h for scope and limits. */
#include "arm64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define M32 0xFFFFFFFFull

/* ---------------------------------------------------------------- helpers */

static int fatal(A64 *c, const char *m, uint64_t v) {
    if (!c->fault)
        snprintf(c->err, sizeof c->err, "%s (value 0x%llx) at pc=0x%llx", m,
                 (unsigned long long)v, (unsigned long long)c->pc);
    c->fault = 1;
    return -1;
}

static uint64_t ones(unsigned n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }
static int64_t sx(uint64_t v, int bits) {
    uint64_t m = 1ull << (bits - 1);
    v &= ones(bits);
    return (int64_t)((v ^ m) - m);
}
static uint64_t ror_w(uint64_t v, unsigned r, unsigned w) {
    v &= ones(w);
    if (!r) return v;
    return ((v >> r) | (v << (w - r))) & ones(w);
}

static uint64_t R(A64 *c, int r, bool sp) { return r == 31 ? (sp ? c->sp : 0) : c->x[r]; }
static void W(A64 *c, int r, uint64_t v, bool sp) {
    if (r == 31) { if (sp) c->sp = v; return; }
    c->x[r] = v;
}

static uint64_t umulh(uint64_t a, uint64_t b) {
    uint64_t a0 = a & M32, a1 = a >> 32, b0 = b & M32, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & M32) + (p10 & M32);
    return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
static uint64_t smulh(uint64_t a, uint64_t b) {
    uint64_t r = umulh(a, b);
    if ((int64_t)a < 0) r -= b;
    if ((int64_t)b < 0) r -= a;
    return r;
}

/* ------------------------------------------------------------ time / IRQ */

uint64_t a64_counter(const A64 *c) { return c->steps + c->cnt_skip; }
void a64_advance_time(A64 *c, uint64_t t) { c->cnt_skip += t; }

static bool timer_asserted(const A64 *c) {
    return (c->cntv_ctl & 1) && a64_counter(c) >= c->cntv_cval;
}
bool a64_irq_asserted(const A64 *c) {
    return c->irq_line || (!(c->cntv_ctl & 2) && timer_asserted(c));
}
/* MODELLED (0.4.33): the EL1 physical timer (CNTP) is delivered as a FIQ, as on Apple SoCs (the kernel's decrementer). Real hardware
   gates this with Apple-specific registers; here only the architectural enable/mask bits of CNTP_CTL_EL0 count. */
static bool fiq_asserted(const A64 *c) {
    return (c->cntp_ctl & 3) == 1 && a64_counter(c) >= c->cntp_cval;
}
static uint64_t timer_wait_ticks(const A64 *c) {      /* ticks until the nearest enabled, unmasked timer fires (0 = none) */
    uint64_t now = a64_counter(c), best = 0;
    if ((c->cntv_ctl & 3) == 1) best = c->cntv_cval > now ? c->cntv_cval - now : 1;
    if ((c->cntp_ctl & 3) == 1) { uint64_t d = c->cntp_cval > now ? c->cntp_cval - now : 1; if (!best || d < best) best = d; }
    return best;
}

/* ------------------------------------------------------- stack / EL state */

static int sp_idx(const A64 *c) { return (c->el == 0 || !c->spsel) ? 0 : 1; }
static void set_el_sp(A64 *c, int el, int spsel) {
    c->sp_bank[sp_idx(c)] = c->sp;
    c->el = el; c->spsel = spsel;
    c->sp = c->sp_bank[sp_idx(c)];
}
static uint64_t pstate_pack(const A64 *c) {
    uint64_t s = ((uint64_t)c->n << 31) | ((uint64_t)c->z << 30) |
                 ((uint64_t)c->c << 29) | ((uint64_t)c->v << 28) |
                 ((uint64_t)c->daif << 6);
    s |= (uint64_t)c->el << 2;
    if (c->el == 1 && c->spsel) s |= 1;
    if (c->pan) s |= 1ull << 22;
    return s;
}

/* Take an exception to EL1.  kind: 0 sync, 1 IRQ.  Always returns 1. */
static int fatal(A64 *c, const char *msg, uint64_t val);
static bool peek64(A64 *c, uint64_t va, uint64_t *out);
static int take_exc(A64 *c, int kind, uint32_t esr, uint64_t ret_pc) {
    if (c->mode == A64_MODE_SYSTEM && c->el == 1 && c->exc_count && ret_pc >= c->vbar_el1 && ret_pc < c->vbar_el1 + 0x800) {
        char m[300];
        snprintf(m, sizeof m, "exception inside the exception vector itself (the handler at 0x%llx faulted again: new ESR=0x%x FAR=0x%llx; first exception ESR_EL1=0x%llx FAR_EL1=0x%llx; sp=0x%llx) - endless loop avoided. Cause of first exception: %s",
                 (unsigned long long)ret_pc, esr, (unsigned long long)c->far_el1, (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1, (unsigned long long)c->sp,
                 c->trap_note[0] ? c->trap_note : "see ESR_EL1");
        return fatal(c, m, esr);
    }
    if (c->mode == A64_MODE_SYSTEM && kind == 0) {          /* synchronous exceptions only: interrupts are legitimately frequent */
        if ((esr >> 26) == 0x15) { if (c->svc_n < 96) { c->svc_nr[c->svc_n] = (uint32_t)c->x[16]; c->svc_pc[c->svc_n] = ret_pc; } c->svl_nr[c->svc_n & 4095] = (uint32_t)c->x[16]; c->svl_pc[c->svc_n & 4095] = ret_pc; c->svl_x0[c->svc_n & 4095] = c->x[0]; c->svl_x1[c->svc_n & 4095] = c->x[1]; c->svl_x2[c->svc_n & 4095] = c->x[2]; c->svl_x3[c->svc_n & 4095] = c->x[3]; c->svl_ret[c->svc_n & 4095] = ~0ull - 1; c->svl_ttbr[c->svc_n & 4095] = c->ttbr0_el1; c->svc_pend = c->svc_n + 1; if (((int)c->x[16] == 4 || (int)c->x[16] == 397) && (c->x[0] == 1 || c->x[0] == 2) && c->x[2] > 0) { unsigned wi = c->wr_n++ & 31; uint64_t nb = c->x[2] > 128 ? 128 : c->x[2]; unsigned k = 0; for (uint64_t o = 0; o < nb; o += 8) { uint64_t w; if (!peek64(c, c->x[1] + o, &w)) break; for (int q = 0; q < 8 && o + q < nb; q++) c->wr_txt[wi][k++] = (char)(w >> (8 * q)); } c->wr_txt[wi][k] = 0; c->wr_ttbr[wi] = c->ttbr0_el1; c->wr_fd[wi] = (unsigned)c->x[0]; }   /* 0.4.406: what user processes write to the console */
        c->svc_pend = c->svc_n + 1; { unsigned si = c->svc_n & 4095; for (int q = 0; q < 12; q++) c->svl_str[si][q] = 0; int sn = (int)c->x[16]; uint64_t pa0 = (sn == 244) ? c->x[1] : (sn == 59) ? c->x[0] : c->x[0]; if (sn == 5 || sn == 338 || sn == 189 || sn == 244 || sn == 59) for (int q = 0; q < 12; q++) { uint64_t w; if (!peek64(c, pa0 + 8u * (uint64_t)q, &w)) break; c->svl_str[si][q] = w; } } c->svc_n++; }   /* 0.4.451: 96-byte paths, + posix_spawn(244, path in x1) / execve(59) */
        if ((esr >> 26) == 0x20 || (esr >> 26) == 0x24) { unsigned q = c->el0l_n++ & 7; c->el0l[q].esr = esr; c->el0l[q].elr = ret_pc; c->el0l[q].far = c->far_el1; c->el0l[q].steps = c->steps; c->el0l[q].x0 = c->x[0]; c->el0l[q].x1 = c->x[1]; c->el0l[q].sp0 = c->sp; c->el0l[q].ttbr = c->ttbr0_el1; }
        if ((esr >> 26) == 0x24 && c->cov_len) { if (!c->ex_seen) c->ex_seen = (uint8_t *)calloc(c->cov_len / 32 + 1, 1); else memset(c->ex_seen, 0, c->cov_len / 32 + 1); c->ex_n = 0; c->ex_far = c->far_el1; c->ex_thr = c->tpidr_el1; c->ex_ri = 0; }   /* 0.4.397: new fault excursion: forget the first-call set */
        if ((esr >> 26) == 0x20 || (esr >> 26) == 0x24) { c->el0_last_ttbr = c->ttbr0_el1; memcpy(c->el0r.x, c->x, sizeof c->el0r.x); c->el0r.codeva = (ret_pc & ~15ull) - 16; c->el0r.tp0 = c->tpidr_el0; c->el0r.tp1 = c->tpidr_el1; c->el0r.tpro = c->tpidrro_el0; c->el0r.sp = c->sp; c->el0r.frn = 0; { uint64_t fp = c->x[29]; while (c->el0r.frn < 12 && fp) { uint64_t nfp, lr; if (!peek64(c, fp, &nfp) || !peek64(c, fp + 8, &lr)) break; c->el0r.fr[c->el0r.frn][0] = fp; c->el0r.fr[c->el0r.frn][1] = lr; c->el0r.frn++; if (nfp <= fp) break; fp = nfp; } } for (int q = 0; q < 24; q++) if (!peek64(c, c->sp + 8u * (uint64_t)q, &c->el0r.stk[q])) c->el0r.stk[q] = 0xdead; c->el0r.ok = 1; for (int q = 0; q < 8; q++) if (!peek64(c, c->el0r.codeva + 8u * (uint64_t)q, &c->el0r.code[q])) { c->el0r.ok = 0; break; } }
        if (((esr >> 26) == 0x20 || (esr >> 26) == 0x24) && c->exc_el0_n < 12) { unsigned q = c->exc_el0_n++; c->exc_el0[q].esr = esr; c->exc_el0[q].elr = ret_pc; c->exc_el0[q].far = c->far_el1; c->exc_el0[q].steps = c->steps; c->exc_el0[q].x0 = c->x[0]; c->exc_el0[q].x1 = c->x[1]; c->exc_el0[q].sp0 = c->sp; }
        if (c->exc_first_n < 8) {
            int k = c->exc_first_n++;
            if (!c->exc0_valid && (esr >> 26) == 0x00) {   /* 0.4.355: capture the first EC=0 (undefined-instruction) exception = the kernel's panic trap (earlier: the first exception that is not a kernel-VA translation fault) */  c->exc0_k = k; memcpy(c->exc0_x, c->x, sizeof c->exc0_x); c->exc0_sp = c->sp; c->exc0_valid = true; for (int r = 0; r < 8; r++) { c->exc0_qok[r] = 0; if (c->x[r] >= 0xffffffe000000000ull && !(c->x[r] & 7)) { int ok = 1; for (int q = 0; q < 16; q++) if (!peek64(c, c->x[r] + 8u * (uint64_t)q, &c->exc0_q[r][q])) { ok = 0; break; } c->exc0_qok[r] = (uint8_t)ok; } } { unsigned na = c->acc_i < A64_ACC_HIST ? c->acc_i : A64_ACC_HIST; c->exc0_acc_n = na; for (unsigned q = 0; q < na; q++) c->exc0_acc[q] = c->acc[(c->acc_i - na + q) % A64_ACC_HIST]; } { uint64_t fp = c->x[29]; c->exc0_chain_n = 0; while (c->exc0_chain_n < 8 && fp) { uint64_t nfp, lr; if (!peek64(c, fp, &nfp) || !peek64(c, fp + 8, &lr)) break; c->exc0_chain[c->exc0_chain_n][0] = fp; c->exc0_chain[c->exc0_chain_n][1] = lr; c->exc0_chain_n++; if (nfp <= fp) break; fp = nfp; } } { unsigned n = c->pc_hist_i < 48 ? c->pc_hist_i : 48; c->exc0_pcn = n; for (unsigned q = 0; q < n; q++) c->exc0_pc[q] = c->pc_hist[(c->pc_hist_i - n + q) % A64_PC_HIST]; } }
            c->exc_first[k].esr = kind == 0 ? esr : 0; c->exc_first[k].kind = kind; c->exc_first[k].elr = ret_pc; c->exc_first[k].far = c->far_el1; c->exc_first[k].steps = c->steps;
        }
        if (c->steps - c->exc_last_step < 400) c->exc_burst++; else c->exc_burst = 0;
        c->exc_last_step = c->steps;
        if (c->exc_burst >= 6) {
            char m[300];
            snprintf(m, sizeof m, "exception storm: 6 exceptions in a row, each within 400 instructions of the previous one (last ESR=0x%x ELR=0x%llx FAR=0x%llx, sp=0x%llx). Cause of the first exception: %s",
                     esr, (unsigned long long)ret_pc, (unsigned long long)c->far_el1, (unsigned long long)c->sp, c->trap_note[0] ? c->trap_note : "see 'first exceptions' below");
            return fatal(c, m, esr);
        }
    }
    uint64_t off;
    if (c->el == 0) off = 0x400;
    else off = c->spsel ? 0x200 : 0x000;
    off += (uint64_t)kind * 0x80;
    c->spsr_el1 = pstate_pack(c);
    c->pan = !(c->sctlr_el1 & (1ull << 23));         /* SCTLR_EL1.SPAN = 0: PAN is set on exception entry */
    c->elr_el1 = ret_pc;
    if (kind == 0) c->esr_el1 = esr;
    set_el_sp(c, 1, 1);
    c->daif = 0xF;
    c->excl_valid = false;
    c->pc = c->vbar_el1 + off;
    c->exc_count++;
    return 1;
}

enum { EC_UNKNOWN = 0x00, EC_SYSREG = 0x18, EC_SVC = 0x15, EC_IABT_LOW = 0x20, EC_IABT = 0x21,
       EC_PCALIGN = 0x22, EC_DABT_LOW = 0x24, EC_DABT = 0x25, EC_BRK = 0x3C };
#define ESR(ec, iss) (((uint32_t)(ec) << 26) | (1u << 25) | ((uint32_t)(iss) & 0x1FFFFFFu))

/* Guest-visible failure: exception in system mode, fatal emulator fault in user mode. */
static int undef(A64 *c, uint32_t insn) {
    if (c->mode == A64_MODE_SYSTEM) return take_exc(c, 0, ESR(EC_UNKNOWN, 0), c->pc);
    return fatal(c, "undefined/unsupported instruction", insn);
}
static int abort_data(A64 *c, uint64_t addr, bool wr, unsigned fsc) {
    if (c->mode != A64_MODE_SYSTEM)
        return fatal(c, wr ? "store to unmapped address" : "load from unmapped address", addr);
    c->far_el1 = addr;
    return take_exc(c, 0, ESR(c->el ? EC_DABT : EC_DABT_LOW, (fsc & 0x3F) | (wr ? 1u << 6 : 0)), c->pc);
}

/* ----------------------------------------------------------------- memory */

/* ------------------------------------------------------------------ MMU
 * Stage-1 translation for EL1&0 with 4K/16K/64K granules, TTBR0/TTBR1, T0SZ/T1SZ, TBI, table/block/page
 * descriptors, access flag (hardware update when TCR_EL1.HA, otherwise an access-flag fault).
 * NOT implemented: permission checks (AP/XN/PXN/PAN are ignored, which also sidesteps Apple's APRR re-meaning of
 * those bits), APTable/XNTable, ASIDs (the TLB is flushed on TTBR writes), 52-bit addressing, stage 2, MTE.
 * Fault status codes follow the architecture: 0x04|level translation, 0x08|level access flag, 0x00|level address size. */
static uint8_t *ptr(A64 *c, uint64_t a, uint64_t n);
#define MMU_ON(c) ((c)->mode == A64_MODE_SYSTEM && ((c)->sctlr_el1 & 1))

static void tlb_flush(A64 *c) { memset(c->tlb_tag, 0, sizeof c->tlb_tag); c->if_valid = false; }

static int walk(A64 *c, uint64_t va, uint64_t *pa, bool update, uint8_t *ro) {
    uint64_t tcr = c->tcr_el1;
    int sel = (int)((va >> 55) & 1);
    int tsz = (int)(sel ? (tcr >> 16) & 63 : tcr & 63);
    int tg = (int)(sel ? (tcr >> 30) & 3 : (tcr >> 14) & 3);
    int g = sel ? (tg == 1 ? 14 : tg == 2 ? 12 : tg == 3 ? 16 : 0) : (tg == 0 ? 12 : tg == 1 ? 16 : tg == 2 ? 14 : 0);
    bool epd = (tcr >> (sel ? 23 : 7)) & 1, tbi = (tcr >> (sel ? 38 : 37)) & 1;
    uint64_t ttbr = sel ? c->ttbr1_el1 : c->ttbr0_el1;
    int ia = 64 - tsz;
    if (!g || epd || ia < 16 || ia > 48) return 0x04;
    uint64_t hi = tbi ? 0x00FFFFFFFFFFFFFFull : ~0ull;
    uint64_t top = (va & hi) >> ia, tmask = hi >> ia;
    if (top != (sel ? tmask : 0)) return 0x04;                       /* not canonical for this half */
    int bpl = g - 3, levels = (ia - g + bpl - 1) / bpl, start = 4 - levels;
    int sbits = ia - g - (levels - 1) * bpl;
    int tb = sbits + 3; if (tb < 6) tb = 6;
    uint64_t tbl = (ttbr & 0x0000FFFFFFFFFFFEull) & ~((1ull << tb) - 1);
    for (int lvl = start; lvl <= 3; lvl++) {
        int shift = g + (3 - lvl) * bpl;
        uint64_t idx = (va >> shift) & ((1ull << (lvl == start ? sbits : bpl)) - 1);
        uint8_t *p = ptr(c, tbl + idx * 8, 8);
        if (!p) return 0x00 | lvl;
        uint64_t d; memcpy(&d, p, 8);
        if (!(d & 1)) return 0x04 | lvl;
        bool table = (d >> 1) & 1;
        if (lvl < 3 && table) { tbl = d & 0x0000FFFFFFFFFFFFull & ~((1ull << g) - 1); continue; }
        if (lvl == 3) { if (!table) return 0x04 | lvl; }
        else if (!(g == 12 ? (lvl == 1 || lvl == 2) : lvl == 2)) return 0x04 | lvl;
        if (!(d & (1ull << 10))) {
            if (!((tcr >> 39) & 1)) return 0x08 | lvl;
            if (update) { d |= 1ull << 10; memcpy(p, &d, 8); }
        }
        if (ro) *ro = (uint8_t)(((d >> 7) & 1) | (lvl << 1));   /* AP[2]: read-only (0.4.394) */
        uint64_t oa = d & 0x0000FFFFFFFFFFFFull & ~((1ull << shift) - 1);
        *pa = oa | (va & ((1ull << shift) - 1));
        return -1;
    }
    return 0x04;
}

/* -1 = translated, otherwise the fault status code */
static int xlate(A64 *c, uint64_t va, uint64_t *pa) {
    uint64_t tcr = c->tcr_el1;
    uint64_t v = va;
    if (((va >> 55) & 1) ? ((tcr >> 38) & 1) : ((tcr >> 37) & 1)) v = ((va >> 55) & 1) ? (va | 0xFF00000000000000ull) : (va & 0x00FFFFFFFFFFFFFFull);
    unsigned slot = (unsigned)((v >> 12) & 255);
    uint64_t tag = (v >> 12) | (1ull << 63);
    if (c->tlb_tag[slot] == tag) { *pa = c->tlb_pa[slot] | (v & 0xFFF); c->last_ro = c->tlb_ro[slot]; return -1; }
    uint64_t out; uint8_t ro = 0; int f = walk(c, v, &out, true, &ro);
    if (f >= 0) return f;
    c->last_ro = ro; c->tlb_ro[slot] = ro;
    c->tlb_tag[slot] = tag; c->tlb_pa[slot] = out & ~0xFFFull;
    *pa = out; return -1;
}

/* Debug: human-readable page-table walk for the panic report (no side effects). */
void a64_walk_trace(A64 *c, uint64_t va, char *out, size_t cap) {
    size_t n = 0;
#define P(...) do { if (n < cap) n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__); } while (0)
    uint64_t tcr = c->tcr_el1;
    int sel = (int)((va >> 55) & 1);
    int tsz = (int)(sel ? (tcr >> 16) & 63 : tcr & 63);
    int tg = (int)(sel ? (tcr >> 30) & 3 : (tcr >> 14) & 3);
    int g = sel ? (tg == 1 ? 14 : tg == 2 ? 12 : tg == 3 ? 16 : 0) : (tg == 0 ? 12 : tg == 1 ? 16 : tg == 2 ? 14 : 0);
    uint64_t ttbr = sel ? c->ttbr1_el1 : c->ttbr0_el1;
    int ia = 64 - tsz;
    if (!g || ia < 16 || ia > 48) { P("walk of 0x%llx: unusable TCR (granule/size)", (unsigned long long)va); out[cap - 1] = 0; return; }
    int bpl = g - 3, levels = (ia - g + bpl - 1) / bpl, start = 4 - levels, sbits = ia - g - (levels - 1) * bpl;
    int tb = sbits + 3; if (tb < 6) tb = 6;
    uint64_t tbl = (ttbr & 0x0000FFFFFFFFFFFEull) & ~((1ull << tb) - 1);
    P("walk of 0x%llx via TTBR%d: %dK granule, %d-bit VA, start level %d, root table 0x%llx (%d entries)", (unsigned long long)va, sel, 1 << (g - 10), ia, start, (unsigned long long)tbl, 1 << sbits);
    for (int lvl = start; lvl <= 3; lvl++) {
        int shift = g + (3 - lvl) * bpl;
        uint64_t idx = (va >> shift) & ((1ull << (lvl == start ? sbits : bpl)) - 1);
        uint8_t *p = ptr(c, tbl + idx * 8, 8);
        if (!p) { P("; L%d[%llu] at 0x%llx is outside RAM", lvl, (unsigned long long)idx, (unsigned long long)(tbl + idx * 8)); break; }
        uint64_t d; memcpy(&d, p, 8);
        P("; L%d[%llu]=0x%llx", lvl, (unsigned long long)idx, (unsigned long long)d);
        if (!(d & 1)) { P(" (invalid)"); break; }
        if (lvl < 3 && ((d >> 1) & 1)) { tbl = d & 0x0000FFFFFFFFFFFFull & ~((1ull << g) - 1); continue; }
        P(" (%s)", lvl == 3 ? "page" : "block"); break;
    }
    out[cap - 1] = 0;
#undef P
}

int a64_virt_to_phys(A64 *c, uint64_t va, uint64_t *pa) {
    if (!MMU_ON(c)) { *pa = va; return 0; }
    uint64_t tcr = c->tcr_el1, v = va;
    if (((va >> 55) & 1) ? ((tcr >> 38) & 1) : ((tcr >> 37) & 1)) v = ((va >> 55) & 1) ? (va | 0xFF00000000000000ull) : (va & 0x00FFFFFFFFFFFFFFull);
    return walk(c, v, pa, false, NULL) < 0 ? 0 : -1;
}

static uint8_t *ptr(A64 *c, uint64_t a, uint64_t n) {
    if (a < c->base) return NULL;
    uint64_t o = a - c->base;
    if (o > c->size || n > c->size - o) return NULL;
    return c->mem + o;
}

/* 0.4.79 diagnostic: DeviceTree-property-name probe. Names are compared by FNV-1a hash (collisions are practically irrelevant for a diagnostic). */
static uint32_t nm_fnv(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h ? h : 1; }
void a64_nm_add_dt_name(A64 *c, const char *name) {
    uint32_t h = nm_fnv(name); unsigned k = h & 16383;
    for (unsigned t = 0; t < 16384; t++, k = (k + 1) & 16383) { if (c->nm_dth[k] == h) break; if (!c->nm_dth[k]) { c->nm_dth[k] = h; break; } }
    c->nm_on = 1;
}
static int nm_has(const uint32_t *tab, unsigned mask, uint32_t h) { unsigned k = h & mask; for (unsigned t = 0; t <= mask; t++, k = (k + 1) & mask) { if (tab[k] == h) return 1; if (!tab[k]) return 0; } return 0; }
static void nm_probe(A64 *c, uint64_t va, uint64_t pc) {
    if (MMU_ON(c) && (va >> 40) != 0xffffffull) return;   /* kernel virtual addresses only (physical when the MMU is off) */
    int inimg = c->cov && va - c->cov_base < c->cov_len;
    if (inimg) { unsigned k = (unsigned)((va >> 2) & 4095); for (unsigned t = 0; t < 4096; t++, k = (k + 1) & 4095) { if (c->nm_seenp[k] == va) return; if (!c->nm_seenp[k]) { c->nm_seenp[k] = va; break; } } }
    if (!inimg) return;   /* 0.4.80: only literal strings of the kernel image; copies on the stack/heap were all memcpy noise in log 0.4.79 */
    uint64_t pa; if (a64_virt_to_phys(c, va, &pa)) return;
    uint64_t room = 4096 - (pa & 0xfff); if (room > 47) room = 47;
    uint8_t *p = ptr(c, pa, room); if (!p) return;
    char b[48]; unsigned n = 0, mark = 0;
    while (n < room && p[n]) { unsigned char ch = p[n]; if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '#' || ch == ',' || ch == '.' || ch == '_' || ch == '-')) return; if (ch == '-' || ch == '#') mark = 1; if (ch == ',' || ch == '.') return; b[n] = (char)ch; n++; }
    if (n < 4 || n >= 40 || n == room || !mark || !((b[0] >= 'a' && b[0] <= 'z') || b[0] == '#')) return;
    b[n] = 0; uint32_t h = nm_fnv(b);
    if (nm_has(c->nm_evh, 8191, h)) return;
    { unsigned k = h & 8191; for (unsigned t = 0; t < 8192; t++, k = (k + 1) & 8191) if (!c->nm_evh[k]) { c->nm_evh[k] = h; break; } }
    if (nm_has(c->nm_dth, 16383, h)) return;
    if (c->nm_n < 256) { memcpy(c->nm[c->nm_n], b, n + 1); c->nm_pc[c->nm_n] = pc; c->nm_n++; }
}
static const A64Dev *find_dev(const A64 *c, uint64_t a, unsigned n) {
    for (int k = 0; k < c->ndev; k++) {
        const A64Dev *d = &c->devs[k];
        if (a >= d->base && a - d->base <= d->size && n <= d->size - (a - d->base)) return d;
    }
    return NULL;
}

/* 0 ok, 1 exception taken, -1 fatal */
static inline void acc_rec(A64 *c, uint64_t va, int sz, uint64_t v, int wr) {
    struct A64Acc *e = &c->acc[c->acc_i++ % A64_ACC_HIST];
    e->pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; e->va = va; e->val = v; e->wr = (uint8_t)wr; e->sz = (uint8_t)(1u << sz);
}
static bool peek64(A64 *c, uint64_t va, uint64_t *out);
static void bh_rec(A64 *c, uint64_t a, int sz, uint64_t v, int wr) {
    if (!c->bh_hi || a + (1u << sz) <= c->bh_lo || a >= c->bh_hi || c->bh_n >= 64) return;
    if (!wr && a != c->bh_lo) return;                       /* loads: only the first word (the free-list link) */
    uint64_t pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST];
    for (unsigned q = 0; q < 4 && q < c->bh_n; q++) { unsigned p = c->bh_n - 1 - q; if (c->bh[p].pc == pc && c->bh[p].lr == c->x[30] && c->bh[p].wr == wr) return; }   /* same loop (up to 4 pcs), same caller */
    unsigned k = c->bh_n++; uint64_t l2 = 0; peek64(c, c->x[29] + 8, &l2);
    c->bh[k].steps = c->steps; c->bh[k].pc = pc; c->bh[k].lr = c->x[30]; c->bh[k].lr2 = l2; c->bh[k].off = a - c->bh_lo; c->bh[k].val = v; c->bh[k].sz = (uint8_t)(1u << sz); c->bh[k].wr = (uint8_t)wr;
}
static void watch_rec(A64 *c, uint64_t a, int sz, uint64_t v, int wr) {
    bh_rec(c, a, sz, v, wr);
    if (!c->watch_hi || a + (1u << sz) <= c->watch_lo || a >= c->watch_hi || c->watch_n >= 24) return;
    unsigned k = c->watch_n++;
    c->watch[k].pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; c->watch[k].addr = a; c->watch[k].val = v; c->watch[k].wr = (uint8_t)wr; c->watch[k].sz = (uint8_t)(1u << sz);
}
static void swb_rec(A64 *c, uint64_t a, int sz, uint64_t v) {
    for (int i = 0; i < 2; i++) {
        if (!c->swb[i].hi || a + (1u << sz) <= c->swb[i].lo || a >= c->swb[i].hi || c->swb[i].n >= 8) continue;
        unsigned k = c->swb[i].n++;
        c->swb[i].e[k].pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; c->swb[i].e[k].addr = a; c->swb[i].e[k].val = v; c->swb[i].e[k].sz = (uint8_t)(1u << sz);
    }
}
static void sw_rec(A64 *c, uint64_t a, int sz, uint64_t v) {
    swb_rec(c, a, sz, v);
    if (!c->sw_hi || a + (1u << sz) <= c->sw_lo || a >= c->sw_hi || c->sw_n >= 16) return;
    unsigned k = c->sw_n++;
    c->sw[k].pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; c->sw[k].addr = a; c->sw[k].val = v; c->sw[k].sz = (uint8_t)(1u << sz);
    if (k < 4) memcpy(c->sw_x[k], c->x, sizeof c->sw_x[k]);
}
static int ld(A64 *c, uint64_t a, int sz, uint64_t *out) {
    uint64_t va = a;
    if (MMU_ON(c)) {
        unsigned n = 1u << sz;
        if ((a & 0xFFF) + n > 0x1000) {                       /* crosses a page: translate each byte */
            uint64_t v = 0;
            for (unsigned k = 0; k < n; k++) { uint64_t b; int r = ld(c, a + k, 0, &b); if (r) return r; v |= b << (8 * k); }
            *out = v; return 0;
        }
        int f = xlate(c, a, &a);
        if (f >= 0) return abort_data(c, va, false, (unsigned)f);
    }
    uint8_t *p = ptr(c, a, 1u << sz);
    if (p) {
        uint64_t v = 0;
        memcpy(&v, p, 1u << sz);   /* host is little-endian */
        watch_rec(c, a, sz, v, 0); acc_rec(c, va, sz, v, 0);
        *out = v;
        return 0;
    }
    const A64Dev *d = find_dev(c, a, 1u << sz);
    if (d && d->read) { *out = d->read(d->ud, a - d->base, 1u << sz) & ones(8u << sz); return 0; }
    if (!c->xab_n++) { c->xab_pa = a; c->xab_w = 0; }
    return abort_data(c, va, false, 0x10);                   /* external abort: nothing there */
}
static bool peek64(A64 *c, uint64_t va, uint64_t *out) {
    uint64_t pa = va;
    if (MMU_ON(c)) { if (xlate(c, va, &pa) >= 0) return false; }
    uint8_t *p = ptr(c, pa, 8); if (!p) return false;
    memcpy(out, p, 8); return true;
}
static int st(A64 *c, uint64_t a, int sz, uint64_t v) {
    uint64_t va = a;
    if (MMU_ON(c)) {
        unsigned n = 1u << sz;
        if ((a & 0xFFF) + n > 0x1000) {
            for (unsigned k = 0; k < n; k++) { int r = st(c, a + k, 0, (v >> (8 * k)) & 0xFF); if (r) return r; }
            return 0;
        }
        int f = xlate(c, a, &a);
        if (f >= 0) return abort_data(c, va, true, (unsigned)f);
        /* 0.4.394 MODELLED: stage-1 write permission for user-half (TTBR0) pages. AP[2]=1 means read-only; XNU maps copy-on-write pages that way and relies on the permission fault to copy the page. Kernel-half pages are not checked (Apple APRR re-meaning, not modelled). */
        if ((c->last_ro & 1) && !((va >> 55) & 1)) { c->ro_faults++; return abort_data(c, va, true, 0x0C | (c->last_ro >> 1)); }
    }
    uint8_t *p = ptr(c, a, 1u << sz);
    if (p) { if (c->vw_val && c->vw_n < 8 && ((sz == 3 && v == c->vw_val) || (sz == 2 && v == (c->vw_val & 0xFFFFFFFFull)))) { unsigned k = c->vw_n++; c->vw[k].pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; c->vw[k].va = va; c->vw[k].pa = a; c->vw[k].sz = (uint8_t)(1u << sz); c->vw[k].lr = c->x[30]; for (int q = 0; q < 4; q++) c->vw[k].x[q] = c->x[q]; }
        sw_rec(c, a, sz, v); watch_rec(c, a, sz, v, 1); acc_rec(c, va, sz, v, 1); memcpy(p, &v, 1u << sz); return 0; }
    const A64Dev *d = find_dev(c, a, 1u << sz);
    if (d && d->write) { d->write(d->ud, a - d->base, 1u << sz, v & ones(8u << sz)); return 0; }
    if (!c->xab_n++) { c->xab_pa = a; c->xab_w = 1; }
    return abort_data(c, va, true, 0x10);
}

/* -------------------------------------------------------- ALU primitives */

static uint64_t shiftv(uint64_t v, int t, int a, bool sf) {
    int w = sf ? 64 : 32;
    a &= w - 1;
    if (!sf) v &= M32;
    uint64_t r;
    switch (t) {
    case 0: r = v << a; break;
    case 1: r = v >> a; break;
    case 2: r = sf ? (uint64_t)((int64_t)v >> a)
                   : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> a); break;
    default: r = a ? ((v >> a) | (v << (w - a))) : v; break;
    }
    return sf ? r : (r & M32);
}

/* a + b + cin, optionally setting NZCV. Subtraction is a + ~b + 1. */
static uint64_t addc(A64 *c, uint64_t a, uint64_t b, unsigned cin, bool sf, bool setf) {
    if (!sf) { a &= M32; b &= M32; }
    uint64_t res; bool carry;
    if (sf) {
        uint64_t s = a + b; bool c1 = s < a;
        res = s + cin; carry = c1 || res < s;
    } else {
        uint64_t t = a + b + cin; res = t & M32; carry = (t >> 32) & 1;
    }
    if (setf) {
        int top = sf ? 63 : 31;
        c->n = (res >> top) & 1; c->z = res == 0; c->c = carry;
        c->v = (((a ^ res) & (b ^ res)) >> top) & 1;
    }
    return res;
}

static bool cond(const A64 *c, int cc) {
    bool r;
    switch (cc >> 1) {
    case 0: r = c->z; break;
    case 1: r = c->c; break;
    case 2: r = c->n; break;
    case 3: r = c->v; break;
    case 4: r = c->c && !c->z; break;
    case 5: r = c->n == c->v; break;
    case 6: r = (c->n == c->v) && !c->z; break;
    default: r = true; break;
    }
    if ((cc & 1) && cc != 15) r = !r;
    return r;
}

static bool decode_bitmask(unsigned N, unsigned imms, unsigned immr, bool immediate,
                           unsigned M, uint64_t *wmask, uint64_t *tmask) {
    unsigned x = (N << 6) | (~imms & 63);
    if (!x) return false;
    int len = 0;
    while (x >> (len + 1)) len++;
    if (len < 1 || M < (1u << len)) return false;
    unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    unsigned S = imms & levels, Rr = immr & levels;
    unsigned esize = 1u << len;
    unsigned d = ((S - Rr) & 63) & levels;
    uint64_t welem = ones(S + 1), telem = ones(d + 1);
    welem = ror_w(welem, Rr, esize);
    uint64_t w = 0, t = 0;
    for (unsigned e = 0; e < M; e += esize) { w |= welem << e; t |= telem << e; }
    *wmask = w & ones(M); *tmask = t & ones(M);
    return true;
}

static uint64_t extend_reg(uint64_t v, int opt, int shift) {
    switch (opt & 7) {
    case 0: v &= 0xFF; break;
    case 1: v &= 0xFFFF; break;
    case 2: v &= M32; break;
    case 4: v = (uint64_t)sx(v, 8); break;
    case 5: v = (uint64_t)sx(v, 16); break;
    case 6: v = (uint64_t)sx(v, 32); break;
    default: break;
    }
    return v << shift;
}

static uint64_t bswapw(uint64_t v, int bytes) {
    uint64_t r = 0;
    for (int k = 0; k < bytes; k++) r |= ((v >> (8 * k)) & 0xFF) << (8 * (bytes - 1 - k));
    return r;
}

/* ------------------------------------------------------- user-mode HLE */

static void set_svc_ret(A64 *c, bool bsd, uint64_t val, int err) {
    if (err) {
        if (bsd) { c->x[0] = (uint64_t)err; c->c = true; }
        else c->x[0] = (uint64_t)(-(int64_t)err);
    } else {
        c->x[0] = val;
        if (bsd) c->c = false;
    }
}
/* Linux-arm64 style (x8, svc #0) and XNU/BSD style (x16, svc #0x80). */
static void do_svc(A64 *c, uint32_t imm) {
    bool bsd = imm == 0x80;
    uint64_t num = bsd ? c->x[16] : c->x[8];
    bool is_exit = bsd ? num == 1 : (num == 93 || num == 94);
    bool is_write = bsd ? num == 4 : num == 64;
    bool is_getpid = bsd ? num == 20 : num == 172;
    if (is_exit) { c->halted = true; c->exit_code = (int)c->x[0]; }
    else if (is_write) {
        uint64_t fd = c->x[0], buf = c->x[1], len = c->x[2];
        uint8_t *p = ptr(c, buf, len);
        if (fd != 1 && fd != 2) set_svc_ret(c, bsd, 0, 9);          /* EBADF */
        else if (!p) set_svc_ret(c, bsd, 0, 14);                    /* EFAULT */
        else { if (c->out) c->out(c->ud, (const char *)p, (size_t)len); set_svc_ret(c, bsd, len, 0); }
    } else if (is_getpid) set_svc_ret(c, bsd, 1, 0);
    else {
        if (c->on_unimpl_svc) c->on_unimpl_svc(c->ud, bsd, num);
        set_svc_ret(c, bsd, 0, bsd ? 78 : 38);                      /* ENOSYS */
    }
}

/* -------------------------------------------------------- system registers */

#define SYSKEY(o0, o1, n, m, o2) (((o0) << 14) | ((o1) << 11) | ((n) << 7) | ((m) << 3) | (o2))

/* returns 1 if the register is accessible from EL0 */
static bool sysreg_el0(unsigned k) {
    switch (k) {
    case SYSKEY(3,3,4,2,0): /* NZCV */
    case SYSKEY(3,3,4,4,0): case SYSKEY(3,3,4,4,1): /* FPCR FPSR */
    case SYSKEY(3,3,13,0,2): case SYSKEY(3,3,13,0,3): /* TPIDR_EL0 TPIDRRO_EL0 */
    case SYSKEY(3,3,14,0,0): case SYSKEY(3,3,14,0,1): case SYSKEY(3,3,14,0,2):
    case SYSKEY(3,3,14,3,0): case SYSKEY(3,3,14,3,1): case SYSKEY(3,3,14,3,2):
        return true;
    default: return false;
    }
}

/* 0 ok, 1 = unknown/unsupported register */
/* Pointer-authentication key registers APIA/APIB/APDA/APDB (Lo/Hi) and APGA (Lo/Hi): op0=3 op1=0 CRn=2, CRm=1..3. */
static int pac_key_idx(unsigned k) {
    if ((k >> 14) != 3 || ((k >> 11) & 7) != 0 || ((k >> 7) & 15) != 2) return -1;
    unsigned crm = (k >> 3) & 15, op2 = k & 7;
    if (crm == 1 && op2 < 4) return (int)op2;            /* APIAKeyLo/Hi, APIBKeyLo/Hi */
    if (crm == 2 && op2 < 4) return 4 + (int)op2;        /* APDAKeyLo/Hi, APDBKeyLo/Hi */
    if (crm == 3 && op2 < 2) return 8 + (int)op2;        /* APGAKeyLo/Hi */
    return -1;
}
static int sysreg_read(A64 *c, unsigned k, uint64_t *out) {
    uint64_t cnt = a64_counter(c);
    switch (k) {
    case SYSKEY(3,0,0,0,0): *out = 0x00A64000ull; return 0;            /* MIDR_EL1 (software implementer) */
    case SYSKEY(3,0,0,0,5): *out = c->mpidr ? c->mpidr : 0x80000000ull; return 0;            /* MPIDR_EL1 core 0 */
    case SYSKEY(3,0,0,6,0): *out = 0x0000000000201120ull; return 0;            /* ID_AA64ISAR0: Atomic = 2 (LSE), SHA2 = 1, SHA1 = 1, AES = 2 (AES + PMULL) - exactly what is implemented here */
    case SYSKEY(3,0,0,4,0): *out = 0x00000011ull; return 0;            /* ID_AA64PFR0: EL0/1 A64, FP and AdvSIMD implemented (no FP16) */
    case SYSKEY(3,0,0,7,1): *out = 0x00100000ull; return 0;                 /* ID_AA64MMFR1: PAN = 1 (tracked, not enforced) */
    case SYSKEY(3,0,4,2,3): *out = (uint64_t)c->pan << 22; return 0;        /* PAN */
    case SYSKEY(3,0,0,7,0): *out = 0x2; return 0;                      /* ID_AA64MMFR0: PARange 40 bit */
    case SYSKEY(3,3,4,2,0): *out = ((uint64_t)c->n << 31) | ((uint64_t)c->z << 30) |
                                   ((uint64_t)c->c << 29) | ((uint64_t)c->v << 28); return 0;
    case SYSKEY(3,3,4,2,1): *out = (uint64_t)c->daif << 6; return 0;
    case SYSKEY(3,0,4,2,2): *out = (uint64_t)c->el << 2; return 0;     /* CurrentEL */
    case SYSKEY(3,0,4,2,0): *out = (uint64_t)c->spsel; return 0;       /* SPSel */
    case SYSKEY(3,0,4,1,0): *out = sp_idx(c) == 0 ? c->sp : c->sp_bank[0]; return 0; /* SP_EL0 */
    case SYSKEY(3,0,4,0,0): *out = c->spsr_el1; return 0;
    case SYSKEY(3,0,4,0,1): *out = c->elr_el1; return 0;
    case SYSKEY(3,0,5,2,0): *out = c->esr_el1; return 0;
    case SYSKEY(3,0,6,0,0): *out = c->far_el1; return 0;
    case SYSKEY(3,0,12,0,0): *out = c->vbar_el1; return 0;
    case SYSKEY(3,0,1,0,0): *out = c->sctlr_el1; return 0;
    case SYSKEY(3,0,1,0,2): *out = c->cpacr_el1; return 0;
    case SYSKEY(3,0,2,0,0): *out = c->ttbr0_el1; return 0;
    case SYSKEY(3,0,2,0,1): *out = c->ttbr1_el1; return 0;
    case SYSKEY(3,0,2,0,2): *out = c->tcr_el1; return 0;
    case SYSKEY(3,0,10,2,0): *out = c->mair_el1; return 0;
    case SYSKEY(3,0,13,0,1): *out = c->contextidr_el1; return 0;
    case SYSKEY(3,0,13,0,4): *out = c->tpidr_el1; return 0;
    case SYSKEY(3,3,13,0,2): *out = c->tpidr_el0; return 0;
    case SYSKEY(3,3,13,0,3): *out = c->tpidrro_el0; return 0;
    case SYSKEY(3,3,4,4,0): *out = c->fpcr; return 0;
    case SYSKEY(3,3,4,4,1): *out = c->fpsr; return 0;
    case SYSKEY(3,0,0,0,6): *out = 0; return 0;                        /* REVIDR_EL1 */
    case SYSKEY(3,3,0,0,1): *out = 0x8444C004ull; return 0;            /* CTR_EL0 (64-byte lines) */
    case SYSKEY(3,3,0,0,7): *out = (c->mode == A64_MODE_SYSTEM) ? 0x4 : 0x10; return 0;   /* DCZID_EL0: system mode (MODELLED) 64-byte ZVA block allowed - the iOS kernel's bzero uses DC ZVA; user mode: prohibited */
    case SYSKEY(3,1,0,0,0): *out = 0; return 0;                        /* CCSIDR_EL1 (no cache model) */
    case SYSKEY(3,1,0,0,1): *out = 0; return 0;                        /* CLIDR_EL1 (no caches) */
    case SYSKEY(3,2,0,0,0): *out = c->csselr_el1; return 0;
    case SYSKEY(3,0,1,0,1): *out = c->actlr_el1; return 0;
    case SYSKEY(3,0,10,3,0): *out = c->amair_el1; return 0;
    case SYSKEY(3,0,7,4,0): *out = c->par_el1; return 0;
    case SYSKEY(3,0,14,1,0): *out = c->cntkctl_el1; return 0;
    case SYSKEY(3,3,14,0,0): *out = A64_TIMER_HZ; return 0;            /* CNTFRQ_EL0 */
    case SYSKEY(3,3,14,0,1): case SYSKEY(3,3,14,0,2): *out = cnt; return 0; /* CNTPCT/CNTVCT */
    case SYSKEY(3,3,14,3,1): *out = (c->cntv_ctl & 3) | ((c->cntv_ctl & 1) && cnt >= c->cntv_cval ? 4 : 0); return 0;
    case SYSKEY(3,3,14,3,2): *out = c->cntv_cval; return 0;
    case SYSKEY(3,3,14,3,0): *out = (uint64_t)(uint32_t)(int32_t)(c->cntv_cval - cnt); return 0;
    case SYSKEY(3,3,14,2,1): *out = (c->cntp_ctl & 3) | ((c->cntp_ctl & 1) && cnt >= c->cntp_cval ? 4 : 0); return 0;   /* CNTP_CTL_EL0 */
    case SYSKEY(3,3,14,2,2): *out = c->cntp_cval; return 0;                                                     /* CNTP_CVAL_EL0 */
    case SYSKEY(3,3,14,2,0): *out = (uint64_t)(uint32_t)(int32_t)(c->cntp_cval - cnt); return 0;                /* CNTP_TVAL_EL0 */
    default: break;
    }
    /* reserved ID register space reads as zero */
    if (((k >> 14) == 3) && ((k >> 11) & 7) == 0 && ((k >> 7) & 15) == 0 && ((k >> 3) & 15) >= 4) { *out = 0; return 0; }
    if (pac_key_idx(k) >= 0) { *out = c->pac_key[pac_key_idx(k)]; return 0; }
    /* Debug register space (op0=2, op1=0: OSLAR_EL1, OSLSR_EL1, MDSCR_EL1, DBG*R...): no debug unit is
     * emulated, so it reads as zero and ignores writes.  Kernels clear the OS lock very early. */
    if ((k >> 14) == 2 && ((k >> 11) & 7) == 0) { *out = 0; return 0; }
    if (c->stub_impl_regs && (k >> 14) == 3 && ((k >> 7) & 15) == 15) {      /* S3_x_C15_Cm_y */
        *out = c->on_impl_reg ? c->on_impl_reg(c->impl_ud, k, false, 0) : 0; return 0;
    }
    return 1;
}

static int sysreg_write(A64 *c, unsigned k, uint64_t v) {
    uint64_t cnt = a64_counter(c);
    switch (k) {
    case SYSKEY(3,3,4,2,0): c->n = (v >> 31) & 1; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; return 0;
    case SYSKEY(3,3,4,2,1): c->daif = (v >> 6) & 0xF; return 0;
    case SYSKEY(3,0,4,2,0): set_el_sp(c, c->el, (int)(v & 1)); return 0;
    case SYSKEY(3,0,4,1,0): if (sp_idx(c) == 0) c->sp = v; else c->sp_bank[0] = v; return 0;
    case SYSKEY(3,0,4,0,0): c->spsr_el1 = v; return 0;
    case SYSKEY(3,0,4,2,3): c->pan = (v >> 22) & 1; return 0;
    case SYSKEY(3,0,4,0,1): c->elr_el1 = v; return 0;
    case SYSKEY(3,0,5,2,0): c->esr_el1 = v; return 0;
    case SYSKEY(3,0,6,0,0): c->far_el1 = v; return 0;
    case SYSKEY(3,0,12,0,0): c->vbar_el1 = v & ~0x7FFull; return 0;
    case SYSKEY(3,0,1,0,0): c->sctlr_el1 = v; tlb_flush(c); return 0;     /* bit 0 = MMU enable */
    case SYSKEY(3,0,1,0,2): c->cpacr_el1 = v; return 0;
    case SYSKEY(3,0,2,0,0): c->ttbr0_el1 = v; tlb_flush(c); return 0;
    case SYSKEY(3,0,2,0,1): c->ttbr1_el1 = v; tlb_flush(c); return 0;
    case SYSKEY(3,0,2,0,2): c->tcr_el1 = v; tlb_flush(c); return 0;
    case SYSKEY(3,0,10,2,0): c->mair_el1 = v; return 0;
    case SYSKEY(3,0,13,0,1): c->contextidr_el1 = v; return 0;
    case SYSKEY(3,0,13,0,4): if (c->tp_n < 6) { c->tp_log[c->tp_n][0] = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST]; c->tp_log[c->tp_n][1] = v; c->tp_n++; } c->tpidr_el1 = v; return 0;
    case SYSKEY(3,3,13,0,2): c->tpidr_el0 = v; return 0;
    case SYSKEY(3,3,13,0,3): c->tpidrro_el0 = v; return 0;             /* writable at EL1, read-only at EL0 */
    case SYSKEY(3,2,0,0,0): c->csselr_el1 = v; return 0;
    case SYSKEY(3,0,1,0,1): c->actlr_el1 = v; return 0;
    case SYSKEY(3,0,10,3,0): c->amair_el1 = v; return 0;
    case SYSKEY(3,0,7,4,0): c->par_el1 = v; return 0;
    case SYSKEY(3,0,14,1,0): c->cntkctl_el1 = v; return 0;
    case SYSKEY(3,3,4,4,0): c->fpcr = v; return 0;
    case SYSKEY(3,3,4,4,1): c->fpsr = v; return 0;
    case SYSKEY(3,3,14,3,1): c->cntv_ctl = v & 3; return 0;
    case SYSKEY(3,3,14,3,2): c->cntv_cval = v; return 0;
    case SYSKEY(3,3,14,3,0): c->cntv_cval = cnt + (uint64_t)(int64_t)(int32_t)v; return 0;
    case SYSKEY(3,3,14,2,1): c->cntp_ctl = v & 3; return 0;
    case SYSKEY(3,3,14,2,2): c->cntp_cval = v; return 0;
    case SYSKEY(3,3,14,2,0): c->cntp_cval = cnt + (uint64_t)(int64_t)(int32_t)v; return 0;
    default: break;
    }
    if (pac_key_idx(k) >= 0) { c->pac_key[pac_key_idx(k)] = v; return 0; }
    if ((k >> 14) == 2 && ((k >> 11) & 7) == 0) return 0;     /* debug space: write ignored (see sysreg_read) */
    if (c->stub_impl_regs && (k >> 14) == 3 && ((k >> 7) & 15) == 15) {
        if (c->on_impl_reg) c->on_impl_reg(c->impl_ud, k, true, v);
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------------- execution */

#define TRY(e) do { int r_ = (e); if (r_) return r_; } while (0)

/* access size helper for exclusive / ordered accesses */
static int exec_excl(A64 *c, uint32_t i) {
    int sz = i >> 30, rt = i & 31, rn = (i >> 5) & 31, rs = (i >> 16) & 31;
    bool L = (i >> 22) & 1, o2 = (i >> 23) & 1, o1 = (i >> 21) & 1;
    uint64_t addr = R(c, rn, true);
    if (o1 && o2) {                                  /* CAS / CASP (ARMv8.1 LSE), single core: compare then store */
        uint64_t m = sz == 3 ? ~0ull : ((1ull << (8u << sz)) - 1), old;
        TRY(ld(c, addr, sz, &old));
        if (old == (R(c, rs, false) & m)) TRY(st(c, addr, sz, R(c, rt, false)));
        W(c, rs, old, false);
        return 0;
    }
    if (o1 && !(i & 0x80000000u)) {                  /* CASP: 0 sz 001000 0 L 1 Rs o0 11111 Rn Rt (bit 30 selects 32/64-bit pairs) */
        int es = (i >> 30) & 1 ? 3 : 2;
        if ((rs & 1) || (rt & 1)) return undef(c, i);
        uint64_t m = es == 3 ? ~0ull : 0xFFFFFFFFull, p0, p1;
        TRY(ld(c, addr, es, &p0)); TRY(ld(c, addr + (1u << es), es, &p1));
        if (p0 == (R(c, rs, false) & m) && p1 == (R(c, rs + 1, false) & m)) { TRY(st(c, addr, es, R(c, rt, false))); TRY(st(c, addr + (1u << es), es, R(c, rt + 1, false))); }
        W(c, rs, p0, false); W(c, rs + 1, p1, false);
        return 0;
    }
    if (o1) {                                        /* LDXP/STXP/LDAXP/STLXP */
        int es = (i >> 30) & 1 ? 3 : 2, rt2 = (i >> 10) & 31;
        if (L) { uint64_t v1, v2; TRY(ld(c, addr, es, &v1)); TRY(ld(c, addr + (1u << es), es, &v2)); c->excl_valid = true; c->excl_addr = addr; W(c, rt, v1, false); W(c, rt2, v2, false); }
        else {
            if (c->excl_valid && c->excl_addr == addr) { TRY(st(c, addr, es, R(c, rt, false))); TRY(st(c, addr + (1u << es), es, R(c, rt2, false))); W(c, rs, 0, false); }
            else W(c, rs, 1, false);
            c->excl_valid = false;
        }
        return 0;
    }
    if (!o2) {                                       /* LDXR/LDAXR/STXR/STLXR */
        if (L) {
            uint64_t v; TRY(ld(c, addr, sz, &v));
            c->excl_valid = true; c->excl_addr = addr; W(c, rt, v, false);
        } else {
            if (c->excl_valid && c->excl_addr == addr) {
                TRY(st(c, addr, sz, R(c, rt, false)));
                W(c, rs, 0, false);
            } else W(c, rs, 1, false);
            c->excl_valid = false;
        }
    } else {                                         /* LDAR/STLR (+LORegion forms) */
        if (L) { uint64_t v; TRY(ld(c, addr, sz, &v)); W(c, rt, v, false); }
        else TRY(st(c, addr, sz, R(c, rt, false)));
    }
    return 0;
}

#include "simd.inc"

static int wp_rd(A64 *c, uint64_t a, uint64_t *out) {   /* 0.4.444 diagnostic read: no abort side effects */
    if (a < 0xffffffe000000000ull) return -1;
    uint64_t pa; if (MMU_ON(c)) { int f = xlate(c, a, &pa); if (f >= 0) return -1; } else pa = a;
    uint8_t *p = ptr(c, pa, 8); if (!p) return -1; memcpy(out, p, 8); return 0;
}
static int wp_str(A64 *c, uint64_t a, char *o, int n) { /* printable C string of >=3 chars at a */
    for (int k = 0; k < n - 1; k++) { uint64_t v; if (wp_rd(c, (a + (uint64_t)k) & ~7ull, &v)) return 0; unsigned char ch = (unsigned char)(v >> (8 * ((a + (uint64_t)k) & 7))); if (ch == 0) { o[k] = 0; return k >= 3; } if (ch < 32 || ch > 126) return 0; o[k] = (char)ch; }
    o[n - 1] = 0; return 1;
}

static void wp_snapshot(A64 *c, uint64_t root, int which) {
    char *b = which == 2 ? c->wp_sn[c->wp_sn_n] : which ? c->wp_snap2 : c->wp_snap; size_t cap = which == 2 ? sizeof c->wp_sn[0] : sizeof c->wp_snap; size_t n = 0; char s[56];
    for (int i = 0; i < 8; i++) { uint64_t p1; if (wp_rd(c, root + 8u * (unsigned)i, &p1) || p1 < 0xffffffe000000000ull) continue;
        if (wp_str(c, p1, s, sizeof s)) { n += (size_t)snprintf(b + n, cap - n, " [%d]=\"%s\"", i, s); continue; }
        n += (size_t)snprintf(b + n, cap - n, " [%d]{", i);
        for (int j = 0; j < 8 && n < cap - 300; j++) { uint64_t p2; if (wp_rd(c, p1 + 8u * (unsigned)j, &p2) || p2 < 0xffffffe000000000ull) continue;
            if (wp_str(c, p2, s, sizeof s)) { n += (size_t)snprintf(b + n, cap - n, "%d=\"%s\" ", j, s); continue; }
            n += (size_t)snprintf(b + n, cap - n, "%d(", j);
            for (int w = 0; w < 8 && n < cap - 200; w++) { uint64_t p3; if (wp_rd(c, p2 + 8u * (unsigned)w, &p3) || p3 < 0xffffffe000000000ull) continue;
                if (wp_str(c, p3, s, sizeof s)) { n += (size_t)snprintf(b + n, cap - n, "%d=\"%s\" ", w, s); continue; }
                for (int y = 0; y < 8 && n < cap - 150; y++) { uint64_t p4; if (wp_rd(c, p3 + 8u * (unsigned)y, &p4) || p4 < 0xffffffe000000000ull) continue; if (wp_str(c, p4, s, sizeof s)) n += (size_t)snprintf(b + n, cap - n, "%d.%d=\"%s\" ", w, y, s); } }
            n += (size_t)snprintf(b + n, cap - n, ") "); }
        n += (size_t)snprintf(b + n, cap - n, "}"); }
    if (which == 2) {} else if (which) c->wp_snap2_n = (unsigned)n; else c->wp_snap_n = (unsigned)n;
}
static int exec_ldst(A64 *c, uint32_t i, uint64_t *next) {
    (void)next;
    if ((i >> 26) & 1) return simd_ldst(c, i, next);          /* all SIMD&FP loads/stores have V=1 */
    int rt = i & 31, rn = (i >> 5) & 31;
    if (((i >> 24) & 0x3F) == 0x08) return exec_excl(c, i);
    if ((i & 0x3F200C00u) == 0x38200000u) {          /* ARMv8.1 LSE atomics: LDADD LDCLR LDEOR LDSET LDSMAX LDSMIN LDUMAX LDUMIN SWP (and LDAPR); single core, so load-modify-store */
        int sz = i >> 30, rs = (i >> 16) & 31, o3 = (i >> 15) & 1, opc = (i >> 12) & 7;
        uint64_t addr = R(c, rn, true), m = sz == 3 ? ~0ull : ((1ull << (8u << sz)) - 1), old, nv, sv = R(c, rs, false) & m;
        if (o3 && opc == 4) { if (rs != 31) return undef(c, i); TRY(ld(c, addr, sz, &old)); W(c, rt, old, false); return 0; }   /* LDAPR */
        if (o3 && opc != 0) return undef(c, i);
        TRY(ld(c, addr, sz, &old));
        int bits = 8 << sz; int64_t so = (int64_t)(old << (64 - bits)) >> (64 - bits), ss = (int64_t)(sv << (64 - bits)) >> (64 - bits);
        if (o3) nv = sv;                             /* SWP */
        else switch (opc) {
            case 0: nv = old + sv; break;
            case 1: nv = old & ~sv; break;
            case 2: nv = old ^ sv; break;
            case 3: nv = old | sv; break;
            case 4: nv = so > ss ? old : sv; break;
            case 5: nv = so < ss ? old : sv; break;
            case 6: nv = old > sv ? old : sv; break;
            default: nv = old < sv ? old : sv; break;
        }
        TRY(st(c, addr, sz, nv & m));
        W(c, rt, old, false);
        return 0;
    }
    if ((i & 0xFF200400u) == 0xF8200400u) {          /* LDRAA / LDRAB: load with authenticated base (no check in this model) */
        int64_t off = sx((((i >> 22) & 1) << 9) | ((i >> 12) & 0x1FF), 10) * 8;
        uint64_t base = R(c, rn, true), addr = base + (uint64_t)off, v;
        TRY(ld(c, addr, 3, &v));
        W(c, rt, v, false);
        if (i & 0x800u) W(c, rn, addr, true);
        return 0;
    }

    if (((i >> 24) & 0x3F) == 0x18) {                /* LDR (literal) */
        int opc = i >> 30; uint64_t a = c->pc + (uint64_t)(sx((i >> 5) & 0x7FFFF, 19) * 4);
        uint64_t v;
        if (opc == 3) return 0;                      /* PRFM */
        TRY(ld(c, a, opc == 0 ? 2 : 3, &v));
        if (opc == 2) v = (uint64_t)sx(v, 32);
        W(c, rt, v, false);
        return 0;
    }
    if (((i >> 27) & 7) == 5) {                      /* LDP/STP */
        int opc = i >> 30, type = (i >> 23) & 3, rt2 = (i >> 10) & 31; bool L = (i >> 22) & 1;
        if (opc == 3 || (i >> 26) & 1) return undef(c, i);
        if (opc == 1 && !L) return undef(c, i);
        int sz = opc == 2 ? 3 : 2; int scale = opc == 2 ? 3 : 2;
        int64_t off = sx((i >> 15) & 0x7F, 7) * (1 << scale);
        uint64_t base = R(c, rn, true), addr = type == 1 ? base : base + (uint64_t)off;
        if (L) {
            uint64_t a, b;
            TRY(ld(c, addr, sz, &a)); TRY(ld(c, addr + (1u << sz), sz, &b));
            if (opc == 1) { a = (uint64_t)sx(a, 32); b = (uint64_t)sx(b, 32); }
            W(c, rt, a, false); W(c, rt2, b, false);
        } else {
            TRY(st(c, addr, sz, R(c, rt, false)));
            TRY(st(c, addr + (1u << sz), sz, R(c, rt2, false)));
        }
        if (type == 1) W(c, rn, base + (uint64_t)off, true);
        else if (type == 3) W(c, rn, addr, true);
        return 0;
    }
    if (((i >> 27) & 7) == 7) {                      /* LDR/STR register & immediate forms */
        int sz = i >> 30, opc = (i >> 22) & 3;
        if ((i >> 26) & 1) return undef(c, i);       /* SIMD&FP */
        uint64_t base = R(c, rn, true), addr; int wb = 0; /* 1 = post, 2 = pre */
        if ((i >> 24) & 1) addr = base + (((uint64_t)(i >> 10) & 0xFFF) << sz);
        else if ((i >> 21) & 1) {
            if (((i >> 10) & 3) != 2) return undef(c, i);  /* atomics (LSE) */
            int opt = (i >> 13) & 7, S = (i >> 12) & 1, rm = (i >> 16) & 31;
            if (!(opt & 2)) return undef(c, i);
            addr = base + extend_reg(R(c, rm, false), opt, S ? sz : 0);
        } else {
            int64_t imm = sx((i >> 12) & 0x1FF, 9);
            int t = (i >> 10) & 3;                   /* 0 unscaled, 1 post, 2 unpriv, 3 pre */
            addr = t == 1 ? base : base + (uint64_t)imm;
            wb = t == 1 ? 1 : t == 3 ? 2 : 0;
            if (wb) base = base + (uint64_t)imm;
        }
        if (opc == 0) {
            TRY(st(c, addr, sz, R(c, rt, false)));
        } else {
            if (sz == 3 && opc >= 2) { return 0; }   /* PRFM */
            if (sz == 2 && opc == 3) return undef(c, i);
            uint64_t v; TRY(ld(c, addr, sz, &v));
            if (opc == 2) v = (uint64_t)sx(v, 8 << sz);
            else if (opc == 3) v = (uint64_t)sx(v, 8 << sz) & M32;
            W(c, rt, v, false);
        }
        if (wb) W(c, rn, base, true);
        return 0;
    }
    return undef(c, i);
}

static int exec_sys(A64 *c, uint32_t i, uint64_t *next) {
    int rt = i & 31;
    if (i == 0xD503201Fu) return 0;
    if ((i & 0xFFFFF01Fu) == 0xD503201Fu) {          /* hints */
        unsigned h = (i >> 5) & 0x7F;
        if (h == 3 || h == 2) {                      /* WFI / WFE */
            c->wfi_n++;
            if (c->mode != A64_MODE_SYSTEM || a64_irq_asserted(c) || fiq_asserted(c)) return 0;
            c->wfi_slept++;
            c->idle = true;
            c->idle_ticks = timer_wait_ticks(c);
        }
        return 0;                                    /* NOP, YIELD, SEV, PAC hints ... */
    }
    if ((i & 0xFFFFF01Fu) == 0xD503301Fu) {         /* CLREX, DSB, DMB, ISB, SB */
        if (((i >> 5) & 7) == 2) c->excl_valid = false;
        return 0;
    }
    if ((i & 0xFFF8F01Fu) == 0xD500401Fu) {          /* MSR (immediate) */
        int op1 = (i >> 16) & 7, crm = (i >> 8) & 15, op2 = (i >> 5) & 7;
        if (c->el == 0 && !(op1 == 3)) goto trap;
        if (op1 == 0 && op2 == 5) { set_el_sp(c, c->el, crm & 1); return 0; }
        if (op1 == 0 && op2 == 4) { c->pan = crm & 1; return 0; }    /* MSR PAN, #imm (ARMv8.1): tracked only */
        if (op1 == 0 && op2 == 3) return 0;                          /* MSR UAO, #imm (ARMv8.2): accepted, no effect */
        if (op1 == 3 && op2 == 6) { c->daif |= crm; return 0; }
        if (op1 == 3 && op2 == 7) { c->daif &= ~crm; return 0; }
        return undef(c, i);
    }
    if ((i & 0xFFF80000u) == 0xD5080000u) {          /* SYS: cache maintenance = NOP; TLBI (CRn=8) flushes the whole TLB */
        int sop1 = (i >> 16) & 7, scrn = (i >> 12) & 15, scrm = (i >> 8) & 15, sop2 = (i >> 5) & 7;
        if (c->mode == A64_MODE_SYSTEM && c->el >= 1 && sop1 == 0 && scrn == 7 && scrm == 8 && sop2 <= 3) {   /* AT S1E1R/S1E1W/S1E0R/S1E0W: stage-1 address translation into PAR_EL1 */
            uint64_t va = R(c, rt, false), pa = va; int f = -1;
            if (MMU_ON(c)) f = xlate(c, va, &pa);
            if (f >= 0) c->par_el1 = 1ull | ((uint64_t)(f & 0x3F) << 1);                                  /* F=1, FST = fault status code */
            else c->par_el1 = (pa & 0x0000FFFFFFFFF000ull) | (1ull << 11) | (3ull << 7) | (0xFFull << 56);  /* success: PA, RES1, inner-shareable, Normal WB (permissions are not modelled) */
            c->at_n++; c->at_va = va; c->at_par = c->par_el1;
            return 0;
        }
        if (c->mode == A64_MODE_SYSTEM && sop1 == 3 && scrn == 7 && scrm == 4 && sop2 == 1) {   /* DC ZVA: zero the 64-byte block containing Xt (before 0.4.26 this was a NOP, which left kalloc'd tables dirty) */
            uint64_t a = R(c, rt, false) & ~63ull;
            for (unsigned k = 0; k < 64; k += 8) { int r = st(c, a + k, 3, 0); if (r) return r; }
            return 0;
        }
        if (scrn == 8) tlb_flush(c);
        return 0;
    }
    if ((i & 0xFFE00000u) == 0xD5200000u && (i & 0x00100000u) == 0) return undef(c, i); /* SYSL */
    if ((i & 0xFFF00000u) == 0xD5100000u || (i & 0xFFF00000u) == 0xD5300000u) {
        bool rd = (i >> 21) & 1;
        unsigned k = (((i >> 19) & 1) + 2) << 14 | ((i >> 16) & 7) << 11 | ((i >> 12) & 15) << 7 |
                     ((i >> 8) & 15) << 3 | ((i >> 5) & 7);
        if (c->el == 0 && !sysreg_el0(k)) goto trap;
        if (rd) {
            uint64_t v;
            if (sysreg_read(c, k, &v)) goto trap_unknown;
            W(c, rt, v, false);
        } else {
            int r = sysreg_write(c, k, R(c, rt, false));
            if (r) goto trap_unknown;
        }
        return 0;
    }
    (void)next;
    return undef(c, i);
trap_unknown:
trap:
    if (c->mode != A64_MODE_SYSTEM) return fatal(c, "privileged/unknown system register access", i);
    {
        uint32_t iss = 0;
        if ((i & 0xFFF00000u) == 0xD5300000u || (i & 0xFFF00000u) == 0xD5100000u) {
            iss = ((((i >> 19) & 1) + 2) << 20) | (((i >> 5) & 7) << 17) | (((i >> 16) & 7) << 14) |
                  (((i >> 12) & 15) << 10) | (rt << 5) | (((i >> 8) & 15) << 1) | ((i >> 21) & 1);
        }
        if (iss) snprintf(c->trap_note, sizeof c->trap_note, "%s of unimplemented system register S%u_%u_C%u_C%u_%u (op0_op1_CRn_CRm_op2)",
                          (i >> 21) & 1 ? "MRS" : "MSR", ((i >> 19) & 1) + 2, (i >> 16) & 7, (i >> 12) & 15, (i >> 8) & 15, (i >> 5) & 7);
        return take_exc(c, 0, ESR(EC_SYSREG, iss), c->pc);
    }
}

static int exec(A64 *c, uint32_t i, uint64_t *next) {
    bool sf = i >> 31;
    int rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
    uint64_t pc = c->pc;

    switch ((i >> 25) & 15) {

    /* ------------------------------------------------ data processing (immediate) */
    case 8: case 9: {
        switch ((i >> 23) & 7) {
        case 0: case 1: {                            /* ADR / ADRP */
            int64_t imm = sx(((i >> 5) & 0x7FFFF) << 2 | ((i >> 29) & 3), 21);
            W(c, rd, (i >> 31) ? (pc & ~0xFFFull) + (uint64_t)(imm * 4096) : pc + (uint64_t)imm, false);
            return 0;
        }
        case 2: {                                    /* ADD/SUB immediate */
            bool sub = (i >> 30) & 1, s = (i >> 29) & 1;
            uint64_t imm = (i >> 10) & 0xFFF; if ((i >> 22) & 1) imm <<= 12;
            uint64_t a = R(c, rn, true);
            uint64_t r = sub ? addc(c, a, ~imm, 1, sf, s) : addc(c, a, imm, 0, sf, s);
            W(c, rd, r, !s);
            return 0;
        }
        case 3: return undef(c, i);
        case 4: {                                    /* logical immediate */
            int opc = (i >> 29) & 3; unsigned N = (i >> 22) & 1;
            uint64_t wm, tm;
            if (!sf && N) return undef(c, i);
            if (!decode_bitmask(N, (i >> 10) & 63, (i >> 16) & 63, true, sf ? 64 : 32, &wm, &tm)) return undef(c, i);
            uint64_t a = R(c, rn, false), r;
            switch (opc) { case 0: case 3: r = a & wm; break; case 1: r = a | wm; break; default: r = a ^ wm; }
            if (!sf) r &= M32;
            if (opc == 3) { c->n = sf ? (r >> 63) : ((r >> 31) & 1); c->z = r == 0; c->c = 0; c->v = 0; }
            W(c, rd, r, opc != 3);
            return 0;
        }
        case 5: {                                    /* MOVN/MOVZ/MOVK */
            int opc = (i >> 29) & 3, hw = (i >> 21) & 3; uint64_t imm = (i >> 5) & 0xFFFF;
            if (opc == 1 || (!sf && hw > 1)) return undef(c, i);
            int sh = hw * 16; uint64_t v;
            if (opc == 0) v = ~(imm << sh);
            else if (opc == 2) v = imm << sh;
            else v = (R(c, rd, false) & ~(0xFFFFull << sh)) | (imm << sh);
            if (!sf) v &= M32;
            W(c, rd, v, false);
            return 0;
        }
        case 6: {                                    /* SBFM / BFM / UBFM */
            int opc = (i >> 29) & 3; unsigned N = (i >> 22) & 1, immr = (i >> 16) & 63, imms = (i >> 10) & 63;
            unsigned ds = sf ? 64 : 32; uint64_t wm, tm;
            if (opc == 3 || N != (unsigned)sf || (!sf && (immr > 31 || imms > 31))) return undef(c, i);
            if (!decode_bitmask(N, imms, immr, false, ds, &wm, &tm)) return undef(c, i);
            uint64_t src = R(c, rn, false) & ones(ds), dst = R(c, rd, false) & ones(ds);
            uint64_t rot = ror_w(src, immr, ds), X;
            if (opc == 0) { uint64_t top = ((src >> imms) & 1) ? ones(ds) : 0; X = (top & ~tm) | (rot & wm & tm); }
            else if (opc == 2) X = rot & wm & tm;
            else { uint64_t bot = (dst & ~wm) | (rot & wm); X = (dst & ~tm) | (bot & tm); }
            W(c, rd, X & ones(ds), false);
            return 0;
        }
        default: {                                   /* EXTR */
            unsigned N = (i >> 22) & 1, imms = (i >> 10) & 63; unsigned ds = sf ? 64 : 32;
            if (((i >> 29) & 3) || ((i >> 21) & 1) || N != (unsigned)sf || (!sf && imms > 31)) return undef(c, i);
            uint64_t hi = R(c, rn, false) & ones(ds), lo = R(c, rm, false) & ones(ds), r;
            r = imms == 0 ? lo : ((lo >> imms) | (hi << (ds - imms))) & ones(ds);
            W(c, rd, r, false);
            return 0;
        }
        }
    }

    /* ------------------------------------------------ branches / exceptions / system */
    case 10: case 11: {
        uint32_t top6 = i >> 26;
        if (top6 == 5 || top6 == 0x25) {             /* B / BL */
            if (top6 == 0x25) c->x[30] = pc + 4;
            *next = pc + (uint64_t)(sx(i & 0x3FFFFFF, 26) * 4);
            return 0;
        }
        if ((i & 0xFF000010u) == 0x54000000u) {      /* B.cond */
            if (cond(c, i & 15)) *next = pc + (uint64_t)(sx((i >> 5) & 0x7FFFF, 19) * 4);
            return 0;
        }
        if ((i & 0x7E000000u) == 0x34000000u) {      /* CBZ / CBNZ */
            uint64_t v = R(c, rd, false); if (!sf) v &= M32;
            bool nz = (i >> 24) & 1;
            if ((v == 0) != nz) *next = pc + (uint64_t)(sx((i >> 5) & 0x7FFFF, 19) * 4);
            return 0;
        }
        if ((i & 0x7E000000u) == 0x36000000u) {      /* TBZ / TBNZ */
            unsigned bit = ((i >> 31) << 5) | ((i >> 19) & 31);
            bool nz = (i >> 24) & 1;
            bool set = (R(c, rd, false) >> bit) & 1;
            if (set == nz) *next = pc + (uint64_t)(sx((i >> 5) & 0x3FFF, 14) * 4);
            return 0;
        }
        if ((i & 0xFF000000u) == 0xD4000000u) {      /* exception generation */
            uint32_t imm = (i >> 5) & 0xFFFF;
            switch (i & 0xFFE0001Fu) {
            case 0xD4000001u:                        /* SVC */
                if (c->mode == A64_MODE_SYSTEM) { *next = pc + 4; return take_exc(c, 0, ESR(EC_SVC, imm), pc + 4); }
                do_svc(c, imm);
                return 0;
            case 0xD4200000u:                        /* BRK */
                if (c->mode == A64_MODE_SYSTEM) return take_exc(c, 0, ESR(EC_BRK, imm), pc);
                return fatal(c, "BRK instruction executed", imm);
            case 0xD4400000u:                        /* HLT */
                c->halted = true; c->exit_code = (int)imm; return 0;
            default: return undef(c, i);             /* HVC / SMC / DCPS: no EL2/EL3 */
            }
        }
        if ((i & 0xFFC00000u) == 0xD5000000u) return exec_sys(c, i, next);
        if ((i >> 25) == 0x6B) {                     /* unconditional branch (register) */
            unsigned opc = (i >> 21) & 15, op3 = (i >> 10) & 63;
            if (((i >> 16) & 31) != 31 || (op3 != 0 && op3 != 2 && op3 != 3)) return undef(c, i);
            if (opc == 4) {                          /* ERET / ERETAA / ERETAB */
                if (rn != 31 || (i & 0x1F) != (op3 ? 31u : 0u)) return undef(c, i);
                if (c->mode != A64_MODE_SYSTEM || c->el == 0) return undef(c, i);
                uint64_t s = c->spsr_el1;
                if (s & 0x10) return fatal(c, "ERET to AArch32 is not supported", s);
                int el = (s >> 2) & 3, m = s & 3;
                if (el > 1 || (el == 0 && m != 0)) return fatal(c, "ERET to unsupported exception level/state", s);
                set_el_sp(c, el, el ? (int)(s & 1) : 0);
                c->n = (s >> 31) & 1; c->z = (s >> 30) & 1; c->c = (s >> 29) & 1; c->v = (s >> 28) & 1;
                c->daif = (s >> 6) & 0xF;
                c->pan = (s >> 22) & 1;
                c->excl_valid = false;
                if (el == 0 && c->ex_seen && c->tpidr_el1 == c->ex_thr) { memcpy(c->ex_sr, c->ex_rt, sizeof c->ex_sr); c->ex_sri = c->ex_ri; c->ex_sret = c->elr_el1; c->ex_sx0 = c->x[0]; c->ex_sn++; }   /* 0.4.405: ring of the last return to EL0 */
                if (el == 0 && c->svc_pend) { c->svl_ret[(c->svc_pend - 1) & 4095] = c->x[0]; { uint64_t av = 0; c->svl_aux[(c->svc_pend - 1) & 4095] = peek64(c, c->svl_x1[(c->svc_pend - 1) & 4095], &av) ? av : 0xdeadull; } c->svc_pend = 0; }   /* 0.4.384 diagnostic: x0 at the return to user mode after an SVC */
                *next = c->elr_el1;
                return 0;
            }
            if (op3 && (opc == 8 || opc == 9)) opc &= 1;     /* BRAA/BRAB/BLRAA/BLRAB (modifier in Rm): no signature check */
            if (opc > 2) return undef(c, i);
            if (op3 == 0 && (i & 0x1F)) return undef(c, i);
            if (op3 && opc == 2 && rn != 31) return undef(c, i);
            uint64_t t = (op3 && opc == 2) ? c->x[30] : R(c, rn, false);   /* RETAA/RETAB return through LR */
            if (opc == 2 && c->cap_pc) { c->ret_pc[c->ret_i & 511] = pc; c->ret_x0[c->ret_i & 511] = c->x[0]; c->ret_i++; }
            if (opc == 1) c->x[30] = pc + 4;
            for (int wi = 0; wi < 4; wi++) if (c->wpc[wi] && c->wpc[wi] == pc) { c->wtg[wi] = t & 0xfffffffffull; c->wx0[wi] = c->x[0]; c->wx1[wi] = c->x[1]; c->wn[wi]++; }
            *next = t;
            return 0;
        }
        return undef(c, i);
    }

    /* ------------------------------------------------ loads and stores */
    case 4: case 6: case 12: case 14:
        return exec_ldst(c, i, next);

    /* ------------------------------------------------ SIMD & floating point data processing */
    case 7: case 15:
        return simd_dp(c, i);

    /* ------------------------------------------------ data processing (register) */
    case 5: case 13: {
        if (!((i >> 28) & 1)) {
            int t = (i >> 22) & 3, amt = (i >> 10) & 63;
            if (!((i >> 24) & 1)) {                  /* logical (shifted register) */
                int opc = (i >> 29) & 3; bool N = (i >> 21) & 1;
                if (!sf && amt > 31) return undef(c, i);
                uint64_t b = shiftv(R(c, rm, false), t, amt, sf); if (N) b = ~b;
                uint64_t a = R(c, rn, false), r;
                switch (opc) { case 0: case 3: r = a & b; break; case 1: r = a | b; break; default: r = a ^ b; }
                if (!sf) r &= M32;
                if (opc == 3) { c->n = sf ? (r >> 63) : ((r >> 31) & 1); c->z = r == 0; c->c = 0; c->v = 0; }
                W(c, rd, r, false);
                return 0;
            }
            bool sub = (i >> 30) & 1, s = (i >> 29) & 1;
            if (!((i >> 21) & 1)) {                  /* add/sub (shifted register) */
                if (t == 3 || (!sf && amt > 31)) return undef(c, i);
                uint64_t b = shiftv(R(c, rm, false), t, amt, sf), a = R(c, rn, false);
                W(c, rd, sub ? addc(c, a, ~b, 1, sf, s) : addc(c, a, b, 0, sf, s), false);
                return 0;
            }
            {                                        /* add/sub (extended register) */
                int opt = (i >> 13) & 7, sh = (i >> 10) & 7;
                if (((i >> 22) & 3) || sh > 4) return undef(c, i);
                uint64_t b = extend_reg(R(c, rm, false), opt, sh), a = R(c, rn, true);
                uint64_t r = sub ? addc(c, a, ~b, 1, sf, s) : addc(c, a, b, 0, sf, s);
                W(c, rd, r, !s);
                return 0;
            }
        }
        switch (((i >> 24) & 0x1F) == 0x1B ? 7 : (int)((i >> 21) & 7)) {
        case 0: {                                    /* ADC / SBC */
            if ((i >> 10) & 63) return undef(c, i);
            bool sub = (i >> 30) & 1, s = (i >> 29) & 1;
            uint64_t a = R(c, rn, false), b = R(c, rm, false);
            W(c, rd, addc(c, a, sub ? ~b : b, c->c ? 1 : 0, sf, s), false);
            return 0;
        }
        case 2: {                                    /* CCMP / CCMN */
            if (!((i >> 29) & 1) || ((i >> 10) & 1) || ((i >> 4) & 1)) return undef(c, i);
            bool sub = (i >> 30) & 1;
            if (cond(c, (i >> 12) & 15)) {
                uint64_t a = R(c, rn, false);
                uint64_t b = ((i >> 11) & 1) ? (uint64_t)((i >> 16) & 31) : R(c, rm, false);
                if (sub) addc(c, a, ~b, 1, sf, true); else addc(c, a, b, 0, sf, true);
            } else {
                c->n = (i >> 3) & 1; c->z = (i >> 2) & 1; c->c = (i >> 1) & 1; c->v = i & 1;
            }
            return 0;
        }
        case 4: {                                    /* CSEL / CSINC / CSINV / CSNEG */
            if (((i >> 29) & 1) || ((i >> 11) & 1)) return undef(c, i);
            bool op = (i >> 30) & 1, o2 = (i >> 10) & 1;
            uint64_t r;
            if (cond(c, (i >> 12) & 15)) r = R(c, rn, false);
            else {
                r = R(c, rm, false);
                if (!sf) r &= M32;
                if (op) r = ~r;
                if (o2) r = r + 1;                   /* CSINC: +1, CSNEG: ~x + 1 = -x */
            }
            if (!sf) r &= M32;
            W(c, rd, r, false);
            return 0;
        }
        case 6: {
            if ((i >> 29) & 1) return undef(c, i);
            if ((i >> 30) & 1) {                     /* data-processing (1 source) */
                unsigned op = (i >> 10) & 63; uint64_t v = R(c, rn, false), r;
                if (sf && ((i >> 16) & 31) == 1 && op <= 17) {
                    /* Pointer authentication: PACIA/IB/DA/DB, AUTxx, PACxZx/AUTxZx, XPACI/XPACD.  Model: signatures are
                     * not computed, so PAC* and AUT* leave the pointer unchanged (always authenticates), XPAC* canonicalises. */
                    if (op >= 8 && rn != 31) return undef(c, i);
                    if (op >= 16) { uint64_t p = R(c, rd, false); W(c, rd, ((p >> 55) & 1) ? (p | 0xFF00000000000000ull) : (p & 0x00FFFFFFFFFFFFFFull), false); }
                    return 0;
                }
                if (((i >> 16) & 31) || (!sf && op > 5)) return undef(c, i);
                if (!sf) v &= M32;
                switch (op) {
                case 0: { r = 0; unsigned w = sf ? 64 : 32; for (unsigned k = 0; k < w; k++) if ((v >> k) & 1) r |= 1ull << (w - 1 - k); break; }
                case 1: r = 0; for (unsigned k = 0; k < (sf ? 8u : 4u); k += 2) { uint64_t h = (v >> (8 * k)) & 0xFFFF; r |= ((h >> 8) | ((h & 0xFF) << 8)) << (8 * k); } break;
                case 2: r = sf ? 0 : bswapw(v, 4);
                        if (sf) { r = (bswapw(v & M32, 4)) | (bswapw(v >> 32, 4) << 32); } break;
                case 3: if (!sf) return undef(c, i); r = bswapw(v, 8); break;
                case 4: { unsigned w = sf ? 64 : 32, n = 0; while (n < w && !((v >> (w - 1 - n)) & 1)) n++; r = n; break; }
                case 5: { unsigned w = sf ? 64 : 32, top = w - 1, msb = (v >> top) & 1, n = 0;
                          for (int k = (int)top - 1; k >= 0 && (((v >> k) & 1) == msb); k--) { n++; }
                          r = n; break; }
                default: return undef(c, i);
                }
                W(c, rd, sf ? r : (r & M32), false);
                return 0;
            }
            {                                        /* data-processing (2 source) */
                unsigned op = (i >> 10) & 63; uint64_t a = R(c, rn, false), b = R(c, rm, false), r;
                if (!sf) { a &= M32; b &= M32; }
                switch (op) {
                case 2: r = b ? a / b : 0; break;
                case 3: {
                    int64_t x = sf ? (int64_t)a : (int32_t)a, y = sf ? (int64_t)b : (int32_t)b;
                    if (y == 0) r = 0;
                    else if (y == -1) r = 0 - (uint64_t)x;
                    else r = (uint64_t)(x / y);
                    break; }
                case 12:                                 /* PACGA: deterministic stand-in MAC in the top 32 bits */
                    if (!sf) return undef(c, i);
                    r = ((a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full) ^ c->pac_key[8] ^ (c->pac_key[9] << 1)) & 0xFFFFFFFF00000000ull; break;
                case 8: case 9: case 10: case 11:
                    r = shiftv(a, op - 8, (int)(b & (sf ? 63 : 31)), sf); break;
                default: return undef(c, i);
                }
                W(c, rd, sf ? r : (r & M32), false);
                return 0;
            }
        }
        default:
            if (((i >> 24) & 0x1F) == 0x1B) {        /* data-processing (3 source) */
                if ((i >> 29) & 3) return undef(c, i);
                int ra = (i >> 10) & 31; bool o0 = (i >> 15) & 1; int op31 = (i >> 21) & 7;
                uint64_t a = R(c, rn, false), b = R(c, rm, false), acc = R(c, ra, false), r;
                switch (op31) {
                case 0: { uint64_t p = a * b; r = o0 ? acc - p : acc + p; if (!sf) r &= M32; break; }
                case 1: case 5: {
                    if (!sf) return undef(c, i);
                    uint64_t p = op31 == 1 ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b)
                                           : (a & M32) * (b & M32);
                    r = o0 ? acc - p : acc + p; break; }
                case 2: case 6:
                    if (!sf || o0 || ra != 31) return undef(c, i);
                    r = op31 == 2 ? smulh(a, b) : umulh(a, b); break;
                default: return undef(c, i);
                }
                W(c, rd, r, false);
                return 0;
            }
            return undef(c, i);
        }
    }
    default:
        return undef(c, i);                          /* reserved, SVE, SIMD & FP */
    }
}

static int step(A64 *c) {
    if (c->mode == A64_MODE_SYSTEM && (c->el == 0 || !(c->daif & 2)) && a64_irq_asserted(c)) {
        take_exc(c, 1, 0, c->pc);
        c->steps++;
        return 0;
    }
    if (c->mode == A64_MODE_SYSTEM && (c->el == 0 || !(c->daif & 1)) && fiq_asserted(c)) {
        c->fiq_count++;
        take_exc(c, 2, 0, c->pc);
        c->steps++;
        return 0;
    }
    uint64_t pc = c->pc;
    uint64_t ppc = pc; int ffsc = 0x04;
    uint8_t *p = NULL;
    if (MMU_ON(c)) {
        int f = xlate(c, pc, &ppc);
        if (f >= 0) ffsc = f; else p = ptr(c, ppc, 4);
    } else p = ptr(c, pc, 4);
    if (!p || (pc & 3)) {
        if (c->mode != A64_MODE_SYSTEM) return fatal(c, "bad instruction fetch", pc);
        if (pc >= c->vbar_el1 && pc < c->vbar_el1 + 0x800)
        {
            char m[512]; snprintf(m, sizeof m, "double fault: exception vector table is not reachable (%s). Cause of the first exception: %s. ESR_EL1",
                                  MMU_ON(c) ? "MMU is on: VBAR_EL1 address does not translate to RAM" : "guest never set up VBAR_EL1 in RAM",
                                  c->trap_note[0] ? c->trap_note : "see ESR_EL1");
            return fatal(c, m, c->esr_el1);
        }
        c->far_el1 = pc;
        take_exc(c, 0, ESR((pc & 3) ? EC_PCALIGN : (c->el ? EC_IABT : EC_IABT_LOW), (unsigned)ffsc), pc);
        c->steps++;
        return 0;
    }
    uint32_t i; memcpy(&i, p, 4);
    c->pc_hist[c->pc_hist_i++ % A64_PC_HIST] = pc;
    if (__builtin_expect(c->e6_pc != 0, 0) && pc == 0xfffffff007ed1ca8ull) c->e6_arm = 1;   /* armed when load_init_program starts */
    if (__builtin_expect(c->e6_pc != 0, 0) && c->e6_arm && !c->e6_n && (i & 0xffc00000u) == 0xb9000000u && ((i >> 10) & 0xfff) == 21 && (i & 31) != 31 && ((i >> 5) & 31) != 31 && ((i >> 5) & 31) != 29 && (uint32_t)c->x[i & 31] == 6 && pc >= 0xfffffff007000000ull && pc < 0xfffffff00a800000ull) { c->e6_n = 1; c->e6_lr = c->x[30]; c->e6_x0 = pc; memcpy(c->e6_reg, c->x, sizeof c->e6_reg); for (unsigned hi = 0; hi < 64; hi++) c->e6_hist[hi] = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 64 + hi) % A64_PC_HIST]; }  /* 0.4.370: first kernel-text 32-bit store of 6 to [xN,#84] (b_error = ENXIO); e6_x0 holds the store pc */
    if (__builtin_expect(c->cap_pc != 0, 0) && pc == c->cap_pc && !c->cap_n) { c->cap_n = 1; for (int ci = 0; ci < 4; ci++) c->cap_x[ci] = c->x[ci]; for (unsigned hi = 0; hi < 512; hi++) { c->ret_snap_pc[hi] = c->ret_pc[(c->ret_i + hi) & 511]; c->ret_snap_x0[hi] = c->ret_x0[(c->ret_i + hi) & 511]; } for (unsigned hi = 0; hi < A64_PC_HIST; hi++) c->cap_hist[hi] = c->pc_hist[(c->pc_hist_i + hi) % A64_PC_HIST]; }
    if (__builtin_expect(c->hk_pc != 0, 0) && pc == c->hk_pc && c->x[19] >= 0x1000) { c->hk_n++; (void)st(c, c->x[19] + 0x10b, 1, 1); (void)st(c, c->x[19] + 0x10c, 1, 0); }   /* 0.4.354 MODELLED hook */
    if (__builtin_expect(c->wp_pc != 0, 0) && pc == c->wp_pc && !c->wp_pm_x0 && (c->x[30] & 0xfffffffffull) == 0x814a9f0ull) c->wp_pm_x0 = c->x[0];   /* 0.4.463 */
    if (__builtin_expect(c->fp_pc != 0, 0) && pc == 0xfffffff00814a8c0ull && c->fn_n < 200) { unsigned k = c->fn_n++; c->fn_s[k][0] = 0; if (!wp_str(c, c->x[0], c->fn_s[k], 32)) snprintf(c->fn_s[k], 32, "?"); }   /* 0.4.512 diagnostic only */
    if (__builtin_expect(c->fp_pc != 0, 0) && pc == 0xfffffff00814a7a0ull && c->fq_n < 16 && (c->x[30] & 0xfffffffffull) >= 0x82c0000ull && (c->x[30] & 0xfffffffffull) < 0x8330000ull) { unsigned k = c->fq_n++; c->fq_x0[k] = c->x[0]; c->fq_x1[k] = c->x[1]; c->fq_x2[k] = c->x[2]; c->fq_lr[k] = c->x[30]; c->fq_s[k][0] = 0; { size_t nn = 0; for (unsigned off = 0; off < 0x400 && nn < 100; off += 8) { uint64_t w, sp; char t[24]; if (wp_rd(c, c->x[1] + off, &w) || w < 0xffffffe000000000ull || wp_rd(c, w + 0x10, &sp) || sp < 0xffffffe000000000ull) continue; if (wp_str(c, sp, t, sizeof t)) nn += (size_t)snprintf(c->fq_s[k] + nn, sizeof c->fq_s[k] - nn, "%x=%s ", off, t); } } }   /* 0.4.510 diagnostic only: A7IOP-zone callers of publishFunctionParent(nub, RTBuddy); fq_s = role of x1 */
    if (__builtin_expect(c->fp_pc != 0, 0) && pc == c->fp_pc && c->fp_n < 6 && (c->x[30] & 0xfffffffffull) == 0x8178864ull) { unsigned k = c->fp_n++; c->fp_x0[k] = c->x[0]; c->fp_x1[k] = c->x[1]; c->fp_lr[k] = c->x[30]; c->fp_s[k][0] = 0; if (!wp_str(c, c->x[1], c->fp_s[k], 48)) c->fp_s[k][0] = 0; }   /* 0.4.467 diagnostic only */
    if (__builtin_expect(c->wp2_pc != 0, 0) && pc == c->wp2_pc && c->wp2_n < 8) { unsigned k = c->wp2_n++; c->wp2_x0[k] = c->x[0]; c->wp2_x19[k] = c->x[19]; c->wp2_x22[k] = c->x[22]; }   /* 0.4.460 diagnostic only */
    if (__builtin_expect(c->wp_pc != 0, 0) && pc == c->wp_pc && !c->wp_snap_done && (c->x[30] & 0xfffffffffull) == 0x816bdccull) { c->wp_snap_done = 1; wp_snapshot(c, c->x[0], 0); }   /* 0.4.444: PMGR wait */
    if (__builtin_expect(c->wp_pc != 0, 0) && c->wp_sn_n < 24 && pc == 0xfffffff0082e94dcull) {   /* 0.4.506 diagnostic: RTBuddy::start before the firmware-service wait: this+192 = role object; x0 field shows 'this' */ c->wp_sn[c->wp_sn_n][0] = 0; wp_snapshot(c, c->x[19] + 192, 2); c->wp_sn_lr2[c->wp_sn_n] = c->x[19]; c->wp_sn_n++; }
    if (__builtin_expect(c->wp_pc != 0, 0) && c->wp_sn_n < 24 && ((pc == c->wp_pc && (c->x[30] & 0xfffffffffull) == 0x816bdccull) || (pc == 0xfffffff008075e48ull && (c->x[30] & 0xfffffffffull) == 0x82e94ecull))) {   /* 0.4.492: also the wait of RTBuddy (0x8075e48 called from 0x82e94e8) */ c->wp_snap2_done = 1; c->wp_sn[c->wp_sn_n][0] = 0; wp_snapshot(c, c->x[0], 2); c->wp_sn_lr2[c->wp_sn_n] = c->x[0]; c->wp_sn_n++; }   /* 0.4.449: every resource wait (up to 24) */   /* 0.4.446: second waiter (thread via 81613c0) */
    if (__builtin_expect(c->wp_pc != 0, 0) && pc == c->wp_pc) { unsigned k = (c->wp_n++) & 15; c->wp_x0[k] = c->x[0]; c->wp_x1[k] = c->x[1]; c->wp_lr[k] = c->x[30]; }   /* 0.4.429 diagnostic only */
    /* 0.4.414 MODELLED one-shot call: the first time pc == ch_pc, x1/x2 are set, x30 = pc (the call returns to the same instruction, which then executes normally) and execution continues at ch_target */
    if (__builtin_expect(c->ch_pc != 0, 0) && pc == c->ch_pc && c->ch_n == 0) { c->ch_n = 1; c->x[1] = c->ch_x1; c->x[2] = c->ch_x2; c->x[30] = pc; c->pc = c->ch_target; c->steps++; return 0; }
    if (__builtin_expect(c->jmp_pc != 0, 0) && pc == c->jmp_pc) { c->jmp_n++; c->pc = c->jmp_to; c->steps++; return 0; }
    if (c->cov) {
        uint64_t co = pc - c->cov_base; if (co < c->cov_len) { uint8_t bm = (uint8_t)(1u << ((co >> 2) & 7)); if (!(c->cov[co >> 5] & bm)) { unsigned q = c->nw_n++ % 96; c->nw_pc[q] = pc; c->nw_step[q] = c->steps; c->nw_x0[q] = c->x[0]; c->nw_x1[q] = c->x[1]; c->nw_lr[q] = c->x[30]; } c->cov[co >> 5] |= bm; }
        if ((i & 0xfc000000u) == 0x94000000u) {   /* BL: remember the first call of every target, newest 160 kept */
            int64_t d = (int64_t)(i & 0x3ffffffu); if (d & 0x2000000) d -= 0x4000000;
            uint64_t tg = pc + (uint64_t)(d * 4), to = tg - c->cov_base;
            if (c->ex_seen && c->tpidr_el1 == c->ex_thr && to < c->cov_len) { c->ex_rs[c->ex_ri & 255] = pc; c->ex_rt[c->ex_ri & 255] = tg; c->ex_ri++; }
            if (c->ex_seen && c->tpidr_el1 == c->ex_thr && c->ex_n < 1500 && to < c->cov_len && !(c->ex_seen[to >> 5] & (1u << ((to >> 2) & 7)))) { c->ex_seen[to >> 5] |= (uint8_t)(1u << ((to >> 2) & 7)); c->ex_list[c->ex_n++] = tg; }
            if (to < c->cov_len && !(c->callseen[to >> 5] & (1u << ((to >> 2) & 7)))) { c->callseen[to >> 5] |= (uint8_t)(1u << ((to >> 2) & 7)); c->callring[c->call_total++ % 160] = tg; }
        }
    }
    if (c->nm_on && ((i & 0xfc000000u) == 0x94000000u || (i & 0xfffffc1fu) == 0xd63f0000u || (i & 0xfffff800u) == 0xd73f0800u)) { nm_probe(c, c->x[1], pc); nm_probe(c, c->x[2], pc); }
    if (c->pp2_pc && pc == c->pp2_pc && !c->pp2_hit && c->x[c->pp2_reg & 31] == c->pp2_val) { c->pp2_hit = 1; memcpy(c->pp2_x, c->x, sizeof c->x); c->pp2_sp = c->sp; }
    if (c->pp_pc && pc == c->pp_pc && !c->pp_hit && c->x[c->pp_reg & 31] == c->pp_val) { c->pp_hit = 1; memcpy(c->pp_x, c->x, sizeof c->x); c->pp_sp = c->sp; }
    if (c->tr_pc) { if (!c->tr_on && pc == c->tr_pc) c->tr_on = 1; if (c->tr_on && c->tr_n < 400) c->tr_pcs[c->tr_n++] = pc; }
    if (c->cr_on && (i & 0xfc000000u) == 0x94000000u && (c->daif & 3) == 0 && c->el == 1) { int64_t dd = (int64_t)(i & 0x3ffffffu); if (dd & 0x2000000) dd -= 0x4000000; unsigned q = c->cr_n++ % 64; c->cr_pc[q] = pc; c->cr_tg[q] = pc + (uint64_t)(dd * 4); }
    if (c->snap_pc && pc == c->snap_pc && !c->snap_n) { c->snap_n = 1; c->snap_fp = c->x[29]; c->snap_lr = c->x[30]; c->snap_sp = c->sp; c->snap_x0 = c->x[0]; c->snap_step = c->steps; }   /* 0.4.212 diagnostic: first entry into the panic function */
    if (c->hist_pc && pc == c->hist_pc) { unsigned q = 0; while (q < c->hist_n && !(c->hist_lr[q] == c->x[30] && c->hist_x1[q] == c->x[1])) q++; if (q < c->hist_n) c->hist_cnt[q]++; else if (c->hist_n < 24) { c->hist_lr[q] = c->x[30]; c->hist_x1[q] = c->x[1]; c->hist_cnt[q] = 1; c->hist_n++; } else c->hist_other++; }
    if (c->ctr_pc && pc == c->ctr_pc && c->ctr_mode == 1) { static const int rr[8] = {0, 8, 9, 10, 24, 25, 26, 27}; unsigned slot = c->ctr_total % 40; for (int q = 0; q < 8; q++) c->ctr[slot][q] = c->x[rr[q]]; c->ctr_total++; }
    else if (c->ctr_pc && pc == c->ctr_pc && c->ctr_n < 40 && c->x[2] != 0) { for (int q = 0; q < 7; q++) c->ctr[c->ctr_n][q] = c->x[q]; c->ctr[c->ctr_n][7] = c->x[30]; c->ctr_n++; }
    if (c->aw_val && c->aw_n < 48) {
        uint32_t t6 = i >> 26; char kd = 0; uint64_t tg = 0;
        if (t6 == 0x25) { kd = 'C'; int64_t d = (int64_t)(i & 0x3ffffffu); if (d & 0x2000000) d -= 0x4000000; tg = pc + (uint64_t)(d * 4); }
        else if ((i & 0xFFFFFC1Fu) == 0xD63F0000u) { kd = 'C'; tg = c->x[(i >> 5) & 31]; }
        if (kd == 'C' && (c->x[0] == c->aw_val || c->x[1] == c->aw_val)) { unsigned k = c->aw_n++; c->aw[k].steps = c->steps; c->aw[k].pc = pc; c->aw[k].tgt = tg; c->aw[k].x1 = c->x[1]; c->aw[k].x2 = c->x[2]; c->aw[k].kind = kd; }
        else if ((i == 0xD65F0FFFu || i == 0xD65F0BFFu || (i & 0xFFFFFC1Fu) == 0xD65F0000u) && c->x[0] == c->aw_val) { unsigned k = c->aw_n++; c->aw[k].steps = c->steps; c->aw[k].pc = pc; c->aw[k].tgt = c->x[30]; c->aw[k].x1 = c->x[1]; c->aw[k].x2 = c->x[2]; c->aw[k].kind = 'R'; }
    }
    uint64_t next = pc + 4;
    int r = exec(c, i, &next);
    if (r < 0) return -1;
    if (r == 0) c->pc = next;
    c->steps++;
    return 0;
}

/* -------------------------------------------------------------- public API */

void a64_init(A64 *c, uint8_t *mem, uint64_t base, uint64_t size) {
    memset(c, 0, sizeof *c);
    c->mem = mem; c->base = base; c->size = size;
    c->sp = base + size;
}

void a64_system_init(A64 *c) {
    c->mode = A64_MODE_SYSTEM;
    c->el = 1; c->spsel = 1; c->daif = 0xF;
    c->sp_bank[0] = 0; c->sp_bank[1] = c->sp;
}

int a64_add_device(A64 *c, const A64Dev *d) {
    if (c->ndev >= A64_MAX_DEVS) return -1;
    for (int k = 0; k < c->ndev; k++)
        if (d->base < c->devs[k].base + c->devs[k].size && c->devs[k].base < d->base + d->size) return -1;
    if (d->base < c->base + c->size && c->base < d->base + d->size) return -1;
    c->devs[c->ndev++] = *d;
    return 0;
}

int a64_run(A64 *c, uint64_t max_steps) {
    if (c->fault) return -1;
    if (c->idle) {
        if (!a64_irq_asserted(c) && !fiq_asserted(c)) {
            c->idle_ticks = timer_wait_ticks(c);
            return A64_RUN_IDLE;
        }
        c->idle = false;
    }
    uint64_t limit = c->steps + max_steps;
    while (!c->halted && c->steps < limit) {
        if (step(c)) return -1;
        if (c->idle) return A64_RUN_IDLE;
    }
    return c->halted ? 0 : 1;
}

size_t a64_format_pc_history(const A64 *c, char *buf, size_t cap) {
    size_t n = 0; unsigned cnt = c->pc_hist_i < A64_PC_HIST ? c->pc_hist_i : A64_PC_HIST;
    if (cap) buf[0] = 0;
    for (unsigned k = 0; k < cnt && n + 28 < cap; ) {          /* runs of the same pc are printed once with a count */
        unsigned idx = (c->pc_hist_i - cnt + k) % A64_PC_HIST, run = 1;
        while (k + run < cnt && c->pc_hist[(c->pc_hist_i - cnt + k + run) % A64_PC_HIST] == c->pc_hist[idx]) run++;
        n += (size_t)snprintf(buf + n, cap - n, "%s0x%llx", k ? " " : "", (unsigned long long)c->pc_hist[idx]);
        if (run > 1) n += (size_t)snprintf(buf + n, cap - n, " x%u", run);
        k += run;
    }
    return n;
}
