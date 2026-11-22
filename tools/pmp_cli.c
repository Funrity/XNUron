/* 0.4.537 experiment (option b): run the REAL PMP firmware image (AArch64 RTKit, copied by the kernel into the PMP SRAM; NOT in the repository)
 * on a second core of our interpreter and log every access to addresses nobody models. Usage: pmp_cli image.bin [steps] [load_base]
 * MODELLED/ASSUMPTION: the image is placed at physical load_base (default 0) in a flat 4 MiB RAM; everything else is logged-only MMIO windows. */
#include "../app/src/main/cpp/arm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { char name[24]; unsigned n; } Dv;
static Dv dv[64]; static int ndv;
static A64 *g_c; static uint64_t t_done = ~0ull;
static int timer_pending(void) { return getenv("PMP_TIMER") && g_c && (g_c->cntp_ctl & 1) && a64_counter(g_c) >= g_c->cntp_cval && g_c->cntp_cval != t_done; }
static uint64_t q_lo[64], q_hi[64]; static int q_h, q_t; static uint64_t o_lo;
static void q_push(uint64_t lo, uint64_t hi) { q_lo[q_t % 64] = lo; q_hi[q_t % 64] = hi; q_t++; fprintf(stderr, "[ap->iop] %016llx %llx (MODELLED AP reply)\n", (unsigned long long)lo, (unsigned long long)hi); }
static void on_iop_msg(uint64_t lo, uint64_t hi) { unsigned t = (lo >> 52) & 0xff; fprintf(stderr, "[iop->ap] type %u lo=%016llx hi=%llx\n", t, (unsigned long long)lo, (unsigned long long)hi);
    if (!getenv("PMP_SCRIPT")) return;
    if (t == 1) q_push((2ull << 52) | (10ull << 16) | 10ull, 0);                       /* ASSUMPTION: HELLO_ACK carries ver 10/10 */
    else if (t == 8) q_push((8ull << 52) | (1ull << 51) | 0, 0);                        /* ASSUMPTION placeholder EPMAP reply: filled below */
}
static uint64_t dr(void *u, uint64_t off, unsigned sz) { Dv *d = u; { static int lg; if (off + 0x4000000ull == 0x23bc04810ull && g_c && lg++ < 2) { fprintf(stderr, "[rx-read] pc=%llx lr=%llx", (unsigned long long)g_c->pc, (unsigned long long)g_c->x[30]); uint64_t fp = g_c->x[29]; for (int k = 0; k < 6 && fp > 0x1000000 && fp < 0x1100000; k++) { uint64_t nf, ra; memcpy(&nf, (uint8_t*)g_c->mem + (fp - 0x1000000), 8); memcpy(&ra, (uint8_t*)g_c->mem + (fp - 0x1000000) + 8, 8); fprintf(stderr, " <-%llx", (unsigned long long)ra); fp = nf; } fprintf(stderr, "\n"); } } if (off + 0x4000000ull == 0x23b102004ull && getenv("PMP_AICN") && q_h != q_t) return 0x10000u | atoi(getenv("PMP_AICN")); if (off + 0x4000000ull == 0x23b102004ull && getenv("PMP_T4") && g_c) return 0x40000u; if (off + 0x4000000ull == 0x23b102004ull && g_c) fprintf(stderr, "[tmr-read] pc=%llx lr=%llx\n", (unsigned long long)g_c->pc, (unsigned long long)g_c->x[30]);
    if (getenv("PMP_SCRIPT")) { uint64_t o = off + 0x4000000ull; if (o == 0x23bc0080cull && getenv("PMP_W3")) return q_h == q_t ? 0 : (1u << atoi(getenv("PMP_W3"))); if (o == 0x23bc0081cull && getenv("PMP_AICN") && q_h != q_t) return 0x10000u; if (o == 0x23bc0081cull && timer_pending()) { t_done = g_c->cntp_cval; fprintf(stderr, "[irq] line 0 (MODELLED: CNTP expiry) cval=%llx\n", (unsigned long long)t_done); return 0x10000u; } if (o == 0x23bc0081cull && getenv("PMP_IRQBIT")) return q_h == q_t ? 0 : (0x10000u | atoi(getenv("PMP_IRQBIT"))); if (o == 0x23bc04108ull) return q_h == q_t ? 0x20000 : (1u << 20); if (o == 0x23bc04810ull) return q_h == q_t ? 0 : q_lo[q_h % 64]; if (o == 0x23bc04818ull) { uint64_t v = q_h == q_t ? 0 : q_hi[q_h % 64]; if (q_h != q_t) q_h++; return v; } } if (getenv("PMP_EMPTY") && off == 0x23bc04108ull - 0x4000000ull) { if (d->n++ < 600) fprintf(stderr, "[io] R ctl+4108 -> 0x20000 (MODELLED empty)\n"); return 0x20000; } if (d->n++ < (d->name[0]=='i' ? 600u : 40u)) fprintf(stderr, "[io] R %s+%llx sz%u -> 0\n", d->name, (unsigned long long)off, sz); return 0; }
static void dw(void *u, uint64_t off, unsigned sz, uint64_t v) { Dv *d = u; { uint64_t o = off + 0x4000000ull; if (o == 0x23bc04820ull) o_lo = v; if (o == 0x23bc04828ull) on_iop_msg(o_lo, v); } if (d->n++ < (d->name[0]=='i' ? 600u : 40u)) fprintf(stderr, "[io] W %s+%llx sz%u = %llx\n", d->name, (unsigned long long)off, sz, (unsigned long long)v); }
static void out(void *u, const char *b, size_t n) { (void)u; fwrite(b, 1, n, stderr); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    uint64_t steps = argc > 2 ? strtoull(argv[2], 0, 0) : 2000000, lb = argc > 3 ? strtoull(argv[3], 0, 0) : 0;
    size_t sz = 4u << 20; uint8_t *mem = calloc(1, sz); size_t n = fread(mem + lb, 1, sz - lb, f); fclose(f);
    A64 *c = calloc(1, sizeof *c); a64_init(c, mem, 0, sz); a64_system_init(c);
    c->stub_impl_regs = true; /* ASSUMPTION: Apple impl-defined S3_x_C15 regs read as 0, writes ignored */ c->out = out; c->pc = lb;
    /* logged windows: every 1 MiB page that a 64-bit literal of the image points at (0x2_xxxx_xxxx) */
    uint64_t seen[64]; int ns = 0;
    for (size_t i = 0; i + 8 <= n; i += 4) { uint64_t v; memcpy(&v, mem + lb + i, 8); if (v >= 0x200000000ull && v < 0x300000000ull) { uint64_t p = v & ~0xfffffull; int k = 0; while (k < ns && seen[k] != p) k++; if (k == ns && ns < 60) seen[ns++] = p; } }
    if (getenv("PMP_ALL")) { Dv *d = &dv[ndv++]; snprintf(d->name, sizeof d->name, "io"); A64Dev dd = { 0x4000000ull, 0xFFC000000ull, dr, dw, d, d->name }; if (a64_add_device(c, &dd)) fprintf(stderr, "[io] catch-all window not added\n"); ns = 0; }
    for (int k = 0; k < ns; k++) { Dv *d = &dv[ndv++]; snprintf(d->name, sizeof d->name, "%llx", (unsigned long long)seen[k]); A64Dev dd = { seen[k], 0x100000, dr, dw, d, d->name }; if (a64_add_device(c, &dd)) fprintf(stderr, "[io] window %llx not added\n", (unsigned long long)seen[k]); }
    /* PMP_EXTRA="addr1,addr2,..." : more 1 MiB logged windows (hex), skipped if already present */
    if (getenv("PMP_EXTRA")) { char *e = strdup(getenv("PMP_EXTRA")), *t = strtok(e, ","); while (t) { uint64_t p = strtoull(t, 0, 16) & ~0xfffffull; int k = 0; while (k < ns && seen[k] != p) k++; if (k == ns) { seen[ns++] = p; Dv *d = &dv[ndv++]; snprintf(d->name, sizeof d->name, "%llx", (unsigned long long)p); A64Dev dd = { p, 0x100000, dr, dw, d, d->name }; a64_add_device(c, &dd); } t = strtok(0, ","); } }
    c->watch_lo = 0x4000000ull; c->watch_hi = ~0ull;   /* record accesses above the 4 MiB RAM */
    if (getenv("PMP_HOOK")) { uint64_t hp = strtoull(getenv("PMP_HOOK"), 0, 16); for (uint64_t i = 0; i < steps; i++) { if ((i & 63) == 0) { c->irq_line = ((getenv("PMP_QIRQ") || getenv("PMP_AICN")) && q_h != q_t) || timer_pending(); if (timer_pending()) c->cntp_ctl |= 2; } if (c->pc == hp && getenv("PMP_DUMPTBL")) { uint64_t o = c->x[0] - 0x1000000, nb, ar; memcpy(&nb, mem + o, 4); nb &= 0xffffffff; memcpy(&ar, mem + o + 16, 8); fprintf(stderr, "[tbl] obj=%llx nbuckets=%llu arr=%llx\n", (unsigned long long)c->x[0], (unsigned long long)nb, (unsigned long long)ar); for (uint64_t b = 0; b < nb && b < 256; b++) { uint64_t e; memcpy(&e, mem + ar - 0x1000000 + b * 16, 8); for (int ch = 0; ch < 16 && e > 0x1000000 && e < 0x1100000; ch++) { uint64_t key, val, nx; memcpy(&key, mem + e - 0x1000000, 8); memcpy(&val, mem + e - 0x1000000 + 8, 8); memcpy(&nx, mem + e - 0x1000000 + 16, 8); fprintf(stderr, "[tbl] bucket %llu key=%llx val=%llx\n", (unsigned long long)b, (unsigned long long)key, (unsigned long long)val); e = nx; } } return 0; } if (c->pc == hp) fprintf(stderr, "[hook] step %llu lr=%llx x0=%llx x8=%llx x21=%llx\n", (unsigned long long)i, (unsigned long long)c->x[30], (unsigned long long)c->x[0], (unsigned long long)c->x[8], (unsigned long long)c->x[21]); int rr = a64_run(c, 1); if (rr == A64_RUN_IDLE) { c->irq_line = ((getenv("PMP_QIRQ") || getenv("PMP_AICN")) && q_h != q_t) || timer_pending(); a64_advance_time(c, A64_TIMER_HZ / 1000); } else if (rr != A64_RUN_LIMIT) break; } return 0; }
    if (getenv("PMP_WATCHVA")) { uint64_t va = strtoull(getenv("PMP_WATCHVA"), 0, 16) - 0x1000000, old = 0; memcpy(&old, mem + va, 8);
        for (uint64_t i = 0; i < steps; i++) { uint64_t pc0 = c->pc; if (a64_run(c, 1) != A64_RUN_LIMIT) break; uint64_t nv; memcpy(&nv, mem + va, 8); if (nv != old) { fprintf(stderr, "[watchva] step %llu pc=%llx %llx -> %llx x8=%llx\n", (unsigned long long)i, (unsigned long long)pc0, (unsigned long long)old, (unsigned long long)nv, (unsigned long long)c->x[8]); old = nv; } } return 0; }
    g_c = c;
    uint64_t done = 0; int r = 1;
    unsigned idles = 0;
    while (done < steps) { unsigned ch = getenv("PMP_TIMER") ? 500 : (steps < 1000000 ? steps : 1000000); if (getenv("PMP_TIMER")) { c->irq_line = ((getenv("PMP_QIRQ") || getenv("PMP_AICN")) && q_h != q_t) || timer_pending(); if (timer_pending()) c->cntp_ctl |= 2; /* MODELLED: CNTP expiry reaches the IOP as ASC line 0 (IRQ), not as FIQ */ } r = a64_run(c, ch); if (r == A64_RUN_LIMIT) { done += ch; continue; } if (r == A64_RUN_IDLE && idles < 2000) { c->irq_line = ((getenv("PMP_QIRQ") || getenv("PMP_AICN")) && q_h != q_t) || timer_pending(); if (idles < 5 || idles % 500 == 0) fprintf(stderr, "[pmp] idle #%u at pc=%llx steps=%llu counter=%llu\n", idles, (unsigned long long)c->pc, (unsigned long long)c->steps, (unsigned long long)a64_counter(c)); idles++; a64_advance_time(c, A64_TIMER_HZ / 1000); continue; } break; }
    fprintf(stderr, "[pmp] rc=%d steps=%llu pc=%llx %s\n", r, (unsigned long long)c->steps, (unsigned long long)c->pc, c->err);
    for (unsigned k = 0; k < c->watch_n && k < 24; k++) fprintf(stderr, "[pmp] watch %u: pc=%llx addr=%llx val=%llx %s sz%u\n", k, (unsigned long long)c->watch[k].pc, (unsigned long long)c->watch[k].addr, (unsigned long long)c->watch[k].val, c->watch[k].wr ? "W" : "R", c->watch[k].sz);
    fprintf(stderr, "[pmp] ex_sn=%llu ex_far=%llx\n", (unsigned long long)c->ex_sn, (unsigned long long)c->ex_far); for (unsigned k = 0; k < 8 && k < c->ex_sn; k++) fprintf(stderr, "[pmp] ex %u: rs=%llx rt=%llx sr=%llx\n", k, (unsigned long long)c->ex_rs[k], (unsigned long long)c->ex_rt[k], (unsigned long long)c->ex_sr[k]);
    fprintf(stderr, "[pmp] elr=%llx esr=%llx far=%llx\n", (unsigned long long)c->elr_el1, (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1);
    for (int k = 0; k < 31; k += 1) if (k < 3 || k >= 8 && k <= 10 || k == 19 || k == 20) fprintf(stderr, "[pmp] x%d=%llx\n", k, (unsigned long long)c->x[k]);
    fprintf(stderr, "[pmp] cntp_ctl=%llx cntp_cval=%llx counter=%llx daif=%x\n", (unsigned long long)c->cntp_ctl, (unsigned long long)c->cntp_cval, (unsigned long long)a64_counter(c), c->daif);
    static char h[60000]; a64_format_pc_history(c, h, sizeof h); fprintf(stderr, "[pmp] last pcs: %s\n", h);
    return 0;
}
