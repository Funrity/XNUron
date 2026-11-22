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
static uint64_t dr(void *u, uint64_t off, unsigned sz) { Dv *d = u; if (d->n++ < (d->name[0]=='i' ? 600u : 40u)) fprintf(stderr, "[io] R %s+%llx sz%u -> 0\n", d->name, (unsigned long long)off, sz); return 0; }
static void dw(void *u, uint64_t off, unsigned sz, uint64_t v) { Dv *d = u; if (d->n++ < (d->name[0]=='i' ? 600u : 40u)) fprintf(stderr, "[io] W %s+%llx sz%u = %llx\n", d->name, (unsigned long long)off, sz, (unsigned long long)v); }
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
    if (getenv("PMP_WATCHVA")) { uint64_t va = strtoull(getenv("PMP_WATCHVA"), 0, 16) - 0x1000000, old = 0; memcpy(&old, mem + va, 8);
        for (uint64_t i = 0; i < steps; i++) { uint64_t pc0 = c->pc; if (a64_run(c, 1) != A64_RUN_LIMIT) break; uint64_t nv; memcpy(&nv, mem + va, 8); if (nv != old) { fprintf(stderr, "[watchva] step %llu pc=%llx %llx -> %llx x8=%llx\n", (unsigned long long)i, (unsigned long long)pc0, (unsigned long long)old, (unsigned long long)nv, (unsigned long long)c->x[8]); old = nv; } } return 0; }
    uint64_t done = 0; int r = 1;
    unsigned idles = 0;
    while (done < steps) { r = a64_run(c, steps < 1000000 ? steps : 1000000); if (r == A64_RUN_LIMIT) { done += (steps < 1000000 ? steps : 1000000); continue; } if (r == A64_RUN_IDLE && idles < 2000) { if (idles < 5 || idles % 500 == 0) fprintf(stderr, "[pmp] idle #%u at pc=%llx steps=%llu counter=%llu\n", idles, (unsigned long long)c->pc, (unsigned long long)c->steps, (unsigned long long)a64_counter(c)); idles++; a64_advance_time(c, A64_TIMER_HZ / 1000); continue; } break; }
    fprintf(stderr, "[pmp] rc=%d steps=%llu pc=%llx %s\n", r, (unsigned long long)c->steps, (unsigned long long)c->pc, c->err);
    for (unsigned k = 0; k < c->watch_n && k < 24; k++) fprintf(stderr, "[pmp] watch %u: pc=%llx addr=%llx val=%llx %s sz%u\n", k, (unsigned long long)c->watch[k].pc, (unsigned long long)c->watch[k].addr, (unsigned long long)c->watch[k].val, c->watch[k].wr ? "W" : "R", c->watch[k].sz);
    fprintf(stderr, "[pmp] ex_sn=%llu ex_far=%llx\n", (unsigned long long)c->ex_sn, (unsigned long long)c->ex_far); for (unsigned k = 0; k < 8 && k < c->ex_sn; k++) fprintf(stderr, "[pmp] ex %u: rs=%llx rt=%llx sr=%llx\n", k, (unsigned long long)c->ex_rs[k], (unsigned long long)c->ex_rt[k], (unsigned long long)c->ex_sr[k]);
    fprintf(stderr, "[pmp] elr=%llx esr=%llx far=%llx\n", (unsigned long long)c->elr_el1, (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1);
    for (int k = 0; k < 31; k += 1) if (k < 3 || k >= 8 && k <= 10 || k == 19 || k == 20) fprintf(stderr, "[pmp] x%d=%llx\n", k, (unsigned long long)c->x[k]);
    static char h[60000]; a64_format_pc_history(c, h, sizeof h); fprintf(stderr, "[pmp] last pcs: %s\n", h);
    return 0;
}
