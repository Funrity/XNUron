/*
 * XNUron - ARM64 (AArch64) interpreter core.  Written from scratch.
 *
 * Two operating modes:
 *   A64_MODE_USER   (default)  EL0 user-mode emulation with high-level-emulated
 *                              system calls (Linux: x8 + "svc #0", XNU/BSD:
 *                              x16 + "svc #0x80").  Any guest error is a fatal
 *                              emulator fault (a64_run returns -1).
 *   A64_MODE_SYSTEM            EL0/EL1 system emulation: exception vectors,
 *                              ESR/ELR/SPSR/VBAR, ERET, banked SP, DAIF,
 *                              IRQ delivery, generic virtual timer, system
 *                              registers, MMIO devices.  Guest errors become
 *                              architectural exceptions, like on real hardware.
 *
 * NOT implemented (explicit limits, see docs/FEASIBILITY.md):
 *   MMU / address translation (the guest sees physical addresses; enabling
 *   SCTLR_EL1.M is reported as an emulator fault instead of being ignored),
 *   EL2/EL3, AArch32, FP/SIMD (undefined instruction), LSE atomics, pointer
 *   authentication (PAC instructions behave as unsigned identity), caches/TLB
 *   (maintenance ops are NOPs), multiple cores, Apple-specific system
 *   registers (SPRR/GXF/APRR), and everything an Apple SoC provides.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*A64Out)(void *ud, const char *buf, size_t len);
typedef uint64_t (*A64MmioRead)(void *ud, uint64_t off, unsigned size);
typedef void (*A64MmioWrite)(void *ud, uint64_t off, unsigned size, uint64_t val);
/* Called when user-mode HLE meets a syscall it does not implement. */
typedef void (*A64UnimplSvc)(void *ud, bool bsd, uint64_t num);

typedef struct {
    uint64_t base, size;
    A64MmioRead read;
    A64MmioWrite write;
    void *ud;
    const char *name;
} A64Dev;

enum { A64_MODE_USER = 0, A64_MODE_SYSTEM = 1 };
enum { A64_RUN_HALTED = 0, A64_RUN_LIMIT = 1, A64_RUN_IDLE = 2, A64_RUN_FAULT = -1 };

#define A64_MAX_DEVS 400   /* 0.4.58: was 16 (8 before); every /arm-io node of the DeviceTree can become an unknown block */
#define A64_PC_HIST 4096
#define A64_TIMER_HZ 24000000ull   /* CNTFRQ_EL0; 1 executed instruction == 1 tick */

typedef struct {
    uint64_t x[31];          /* X0..X30 */
    uint64_t sp, pc;         /* sp = currently selected stack pointer */
    bool n, z, c, v;         /* NZCV flags */
    uint8_t *mem;            /* flat guest RAM */
    uint64_t base, size;     /* guest address of mem[0], and its length */
    bool halted;
    int exit_code;
    int fault;               /* 0 = none; non-zero = fatal emulator fault */
    uint64_t steps;          /* instructions executed (also the time base) */
    A64Out out;              /* write(1/2, ...) goes here (user mode HLE) */
    void *ud;
    char err[360];

    /* ---- system mode state ---- */
    int mode;                /* A64_MODE_* */
    int el;                  /* current exception level, 0 or 1 */
    int spsel;               /* PSTATE.SP: 1 = use SP_EL1 at EL1 */
    unsigned daif;           /* D=8 A=4 I=2 F=1 (1 = masked) */
    uint64_t sp_bank[2];     /* SP_EL0, SP_EL1 (the active one lives in sp) */
    uint64_t elr_el1, spsr_el1, esr_el1, far_el1, vbar_el1;
    uint64_t sctlr_el1, tcr_el1, ttbr0_el1, ttbr1_el1, mair_el1, cpacr_el1;
    uint64_t tpidr_el0, tpidr_el1, tpidrro_el0, contextidr_el1;
    uint64_t fpcr, fpsr;
    uint64_t vr[32][2];      /* SIMD&FP registers V0..V31 (little-endian 128 bit) */
    /* MMU (stage 1, EL1&0): software TLB at 4 KiB granularity, keyed by virtual page (bit 63 = valid) */
    uint64_t tlb_tag[256], tlb_pa[256]; uint8_t tlb_ro[256], last_ro; uint64_t ro_faults;
    bool if_valid; uint64_t if_pg, if_pa;        /* last instruction-fetch page */
    uint64_t pac_key[10];    /* PAC key registers (stored, but signatures are not computed: see exec) */
    uint64_t actlr_el1, amair_el1, par_el1, cntkctl_el1, csselr_el1;
    uint64_t cntv_ctl, cntv_cval;
    uint64_t cntp_ctl, cntp_cval;   /* EL1 physical timer (CNTP_*_EL0): registers + ISTATUS only; its interrupt is NOT delivered (Apple routes it as FIQ, not modelled) */
    uint64_t cnt_skip;       /* virtual time added by idle fast-forward */
    bool irq_line;           /* level of the external IRQ input */
    bool idle;               /* executed WFI with nothing to wake it */
    uint64_t idle_ticks;     /* ticks until the timer fires (0 = never) */
    bool pan;                                  /* PSTATE.PAN: tracked and saved/restored, NOT enforced (no permission checks) */
    bool excl_valid; uint64_t excl_addr;
    A64Dev devs[A64_MAX_DEVS]; int ndev;
    /* Apple implementation-defined registers (op0=3, CRn=15): when enabled they read as zero and ignore
     * writes (NOT their real behaviour); each access is reported through on_impl_reg. */
    bool stub_impl_regs; uint64_t (*on_impl_reg)(void *ud, unsigned key, bool write, uint64_t val); void *impl_ud;
    char trap_note[120];     /* last decoded cause of a trapped system-register access */
    uint64_t exc0_q[8][16]; uint8_t exc0_qok[8];   /* 64 bytes behind x0..x7 at the first exception (kernel pointers only) */
    uint64_t pp_pc, pp_val, pp_x[31], pp_sp; unsigned pp_reg; int pp_hit;
    uint64_t pp2_pc, pp2_val, pp2_x[31], pp2_sp; unsigned pp2_reg; int pp2_hit;   /* second pc probe */   /* pc probe: registers captured the first time pc == pp_pc and x[pp_reg] == pp_val */
    uint64_t fiq_count;      /* CNTP FIQs delivered (MODELLED, diagnostics) */
    uint64_t exc_count;      /* exceptions taken (diagnostics) */
    struct { uint32_t esr; int kind; uint64_t elr, far, steps; } exc_first[8];   /* the first exceptions taken (diagnostics) */
    uint64_t exc0_x[31], exc0_sp; bool exc0_valid; uint64_t exc0_pc[A64_PC_HIST]; unsigned exc0_pcn;       /* registers at the very first exception */
    struct { uint32_t esr; uint64_t elr, far, steps, x0, x1, sp0; } exc_el0[12]; unsigned exc_el0_n, svc_n; uint64_t svl_ttbr[4096], el0_last_ttbr; uint64_t svl_str[4096][12]; uint64_t svl_aux[4096], svl_x1[4096], svl_x2[4096], svl_x3[4096], svl_ret[4096]; unsigned svc_pend; uint32_t svl_nr[4096]; uint64_t svl_pc[4096], svl_x0[4096]; struct { uint32_t esr; uint64_t elr, far, steps, x0, x1, sp0, ttbr; } el0l[8]; unsigned el0l_n; struct { uint64_t x[31]; uint64_t code[8]; uint64_t codeva; int ok; uint64_t fr[12][2]; unsigned frn; uint64_t tp0, tp1, tpro, sp, elr, spsr; uint64_t stk[24]; } el0r;   /* full register file and code of the LAST EL0 abort */   /* last 64 SVCs, last 8 EL0 aborts */ uint32_t svc_nr[96]; uint64_t svc_pc[96];   /* 0.4.381: EL0 aborts and the first syscalls of the user process */
    int exc0_k; int exc_first_n; uint64_t exc_last_step; unsigned exc_burst;
    uint64_t pc_hist[A64_PC_HIST]; unsigned pc_hist_i;
    #define A64_ACC_HIST 40
    struct A64Acc { uint64_t pc, va, val; uint8_t wr, sz; } acc[A64_ACC_HIST], exc0_acc[A64_ACC_HIST]; unsigned acc_i, exc0_acc_n;   /* ring of the latest data accesses (diagnostic) */
    uint64_t at_n, at_va, at_par;   /* AT instructions executed: count and the last (va, PAR_EL1) */
    uint64_t mpidr; uint64_t exc0_chain[8][2]; unsigned exc0_chain_n; uint64_t tp_log[6][2]; unsigned tp_n;   /* frame chain at first exception; first TPIDR_EL1 writes (pc,value) */
    struct { uint64_t lo, hi; unsigned n; struct { uint64_t pc, addr, val; uint8_t sz; } e[8]; } swb[2];   /* diagnostic: first stores to two more kernel globals */
    uint64_t xab_pa; unsigned xab_n; int xab_w;   /* diagnostic: first access that hit no RAM and no device (physical address) */
    uint64_t cap_pc, cap_x[4], cap_hist[A64_PC_HIST]; unsigned cap_n;
    uint64_t ret_pc[512], ret_x0[512], ret_snap_pc[512], ret_snap_x0[512]; unsigned ret_i;   /* 0.4.360 diagnostic: ring of (pc of RET, x0); snapshot at cap_pc */   /* 0.4.357 diagnostic: x0..x3 at the first execution of pc==cap_pc */
    uint64_t e6_pc, e6_lr, e6_x0, e6_hist[64], e6_reg[31]; unsigned e6_n, e6_arm;   /* 0.4.369: first execution of pc==e6_pc with x1==6 */
    char wp_snap[6000]; unsigned wp_snap_n; int wp_snap_done; char wp_snap2[6000]; unsigned wp_snap2_n; int wp_snap2_done; char wp_sn[24][420]; unsigned wp_sn_n; uint64_t wp_sn_lr2[24];   /* 0.4.444: string tree of x0 at the moment of the PMGR call */
    uint64_t wp_pm_x0;   /* 0.4.463: x0 of the first wait call from the PMGR client (lr 0x814a9f0) */
    uint64_t wp2_pc; unsigned wp2_n; uint64_t wp2_x0[8], wp2_x19[8], wp2_x22[8];   /* 0.4.460 */
    uint64_t fp_pc; unsigned fp_n; uint64_t fp_x0[6], fp_x1[6], fp_lr[6]; char fp_s[6][48];
    unsigned fq_n; uint64_t fq_x0[16], fq_x1[16], fq_x2[16], fq_lr[16]; char fq_s[16][120];
    unsigned fn_n; char fn_s[200][40];   /* 0.4.512 diagnostic: names built by 0x814a870 */   /* 0.4.507 diagnostic: calls of 0x814a7a0 (function-property setter) */   /* 0.4.467 diagnostic: calls of waitForFunctionParent 0x814a8f0 */
    uint64_t wp_pc; unsigned wp_n; uint64_t wp_x0[16], wp_x1[16], wp_lr[16];   /* 0.4.429 watch: first 16 entries at wp_pc */
    uint64_t ch_pc, ch_target, ch_x1, ch_x2; unsigned ch_n;   /* 0.4.414 MODELLED one-shot call */
    /* 0.4.634 MODELLED retry (ASSUMPTION, see attempt.c): when pc==rt_pc, (uint32)x0==rt_val, byte [x[rt_reg]+rt_off]==0 and fewer than rt_max retries: execution continues at rt_to instead (the instruction at rt_pc is not executed) */
    uint64_t rt_pc, rt_to, rt_off; uint32_t rt_val; unsigned rt_reg, rt_n, rt_max;
    uint64_t jmp_pc, jmp_to; unsigned jmp_n;   /* 0.4.378 MODELLED redirect: when pc==jmp_pc the instruction is NOT executed and execution continues at jmp_to */
    uint64_t hk_pc; unsigned hk_n;   /* 0.4.354 MODELLED hook: when pc==hk_pc, store byte [x19+0x10b]=1 and [x19+0x10c]=0 (see attempt.c; used for the missing SEP key store answer) */
    uint64_t wpc[4], wtg[4], wx0[4], wx1[4]; unsigned wn[4];   /* 0.4.306 diagnostic: for an indirect call at pc==wpc[i] remember the last target (low 36 bits) and the number of executions */
    uint8_t *cov, *callseen, *ex_seen; char wr_txt[32][136]; uint64_t wr_ttbr[32]; unsigned wr_fd[32], wr_n; uint64_t ex_list[1500], ex_far, ex_thr, ex_rs[256], ex_rt[256], ex_sr[256], ex_sret, ex_sx0, ex_sn; unsigned ex_sri; unsigned ex_n, ex_ri; uint64_t cov_base, cov_len; uint64_t callring[160]; unsigned call_total;   /* diagnostic: 1 bit per executed instruction word in [cov_base, cov_base+cov_len) */
    uint64_t tr_pc; unsigned tr_n; int tr_on; uint64_t tr_pcs[400];   /* diagnostic: the 400 pcs executed from the first execution of tr_pc on */
    uint64_t ctr_pc; int ctr_mode; unsigned ctr_total; unsigned ctr_n; uint64_t ctr[40][8];
    uint64_t nw_pc[96]; unsigned nw_n; uint64_t nw_step[96], nw_x0[96], nw_x1[96], nw_lr[96];   /* 0.4.171 diagnostic: ring of the most recently FIRST-executed instructions (the frontier of new code) */
    uint64_t cr_pc[64], cr_tg[64]; unsigned cr_n; int cr_on;   /* 0.4.157 diagnostic: ring of the last bl call sites executed with interrupts unmasked (daif==0) = what the last non-interrupt kernel thread called */
    uint64_t snap_pc, snap_fp, snap_lr, snap_sp, snap_x0, snap_step; unsigned snap_n;   /* diagnostic 0.4.212: snapshot at first entry of snap_pc */
    uint64_t wfi_n, wfi_slept; uint64_t hist_pc; unsigned hist_n; uint64_t hist_lr[24], hist_x1[24], hist_cnt[24], hist_other;   /* diagnostic 0.4.138: histogram of (lr, x1) at every entry of one pc */
       /* diagnostic: x0..x6 and lr at each entry of one function */
    uint64_t vw_val; unsigned vw_n; struct { uint64_t pc, va, pa, lr, x[4]; uint8_t sz; } vw[8];   /* diagnostic: stores of a given value */
    uint64_t sw_lo, sw_hi; unsigned sw_n; struct { uint64_t pc, addr, val; uint8_t sz; } sw[16]; uint64_t sw_x[4][31];   /* diagnostic: first stores to a physical range (kernel global) */
    uint64_t aw_val; unsigned aw_n; struct { uint64_t steps, pc, tgt, x1, x2; char kind; } aw[48];   /* diagnostic: calls with x0/x1 == value, returns with x0 == value */
    uint64_t bh_lo, bh_hi; unsigned bh_n; struct { uint64_t steps, pc, lr, lr2, off, val; uint8_t sz, wr; } bh[64];   /* diagnostic: history of accesses to a physical block */
    uint64_t watch_lo, watch_hi; unsigned watch_n; struct { uint64_t pc, addr, val; uint8_t wr, sz; } watch[24];   /* diagnostic: accesses to a physical range (boot_args) */
    /* 0.4.79 diagnostic: strings passed (x1/x2) to calls that look like DeviceTree property names but are not in the DeviceTree */
    int nm_on; uint32_t nm_dth[16384]; uint32_t nm_evh[8192]; uint64_t nm_seenp[4096]; unsigned nm_n, nm_calls; char nm[256][40]; uint64_t nm_pc[256];
    A64UnimplSvc on_unimpl_svc;
} A64;

void a64_nm_add_dt_name(A64 *c, const char *name);   /* 0.4.79: register a DeviceTree property name (hash set) and switch the probe on */
void a64_init(A64 *c, uint8_t *mem, uint64_t base, uint64_t size);
/* Switch to system mode: EL1h, all DAIF masked, MMU off (reset-like state). */
void a64_system_init(A64 *c);
int  a64_add_device(A64 *c, const A64Dev *d);   /* 0 ok, -1 table full/overlap */

/* returns A64_RUN_HALTED, _LIMIT (step limit hit), _IDLE (WFI, nothing can
 * wake the core until a device raises IRQ or time advances), _FAULT (c->err). */
int a64_run(A64 *c, uint64_t max_steps);
/* Add virtual time (used by hosts that sleep while the guest idles). */
void a64_advance_time(A64 *c, uint64_t ticks);
uint64_t a64_counter(const A64 *c);
/* True when an interrupt is currently asserted towards the core. */
bool a64_irq_asserted(const A64 *c);
/* Debug helper: translates va through the guest page tables exactly like a load would (no side effects).
 * With the MMU off, pa = va. Returns 0 on success, -1 if the address is not mapped. */
int a64_virt_to_phys(A64 *c, uint64_t va, uint64_t *pa);
void a64_walk_trace(A64 *c, uint64_t va, char *out, size_t cap);   /* debug text for panic reports */
/* Formats the last executed PCs (oldest first) for fault diagnostics. */
size_t a64_format_pc_history(const A64 *c, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
