/* Headless runner: the same C sources as the APK, no phone needed.  Usage: boot_cli <ipsw.zip> [max_million_steps]
 * Runs the experimental boot attempt exactly like the app does (a64_attempt_ipsw + a64m_run) and prints the whole log
 * (boot log, kernel output, stop report) to stdout.  Nothing is faked: same stop reasons as on the phone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attempt.h"
#include "machine.h"
#include "arm64.h"
static int file_read(void *ud, uint64_t off, void *buf, size_t len) {
    FILE *f = ud; if (fseeko(f, (off_t)off, SEEK_SET)) return -1; return fread(buf, 1, len, f) == len ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s ipsw.zip [max_million_steps]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 2; }
    fseeko(f, 0, SEEK_END); uint64_t sz = (uint64_t)ftello(f);
    uint64_t maxm = argc > 2 ? strtoull(argv[2], 0, 10) : 4000;
    A64Machine *m = a64_attempt_ipsw(file_read, f, sz); if (!m) { fprintf(stderr, "out of memory\n"); return 2; }
    uint64_t steps = 0; A64MState st;
    { const char *hp = getenv("XN_HIST_PC"); if (hp) a64m_cpu(m)->hist_pc = strtoull(hp, 0, 16); } a64m_cpu(m)->cr_on = 1;   /* 0.4.138 diagnostic */
    while ((st = a64m_run(m, 200000, false)) == A64M_RUNNING) { steps += 200000; if (steps % 500000000ull == 0) { A64 *c0 = a64m_cpu(m); fprintf(stderr, "[progress] %llu M steps: EL%d pc=0x%llx daif=0x%x exceptions=%llu x0=%llx lr=%llx\n", (unsigned long long)(steps / 1000000ull), c0->el, (unsigned long long)c0->pc, c0->daif, (unsigned long long)c0->exc_count, (unsigned long long)c0->x[0], (unsigned long long)c0->x[30]); } if (steps > maxm * 1000000ull) { fprintf(stderr, "step limit reached\n"); break; } }
    a64m_log_flush(m);
    size_t off = 0, n; char *b = malloc(65536);
    while ((n = a64m_log_read(m, off, b, 65536)) > 0) { fwrite(b, 1, n, stdout); off += n; }
    fprintf(stderr, "final state %d, log %zu bytes\n", (int)st, off);
    { A64 *c = a64m_cpu(m);   /* 0.4.133: where is the CPU when the run ends? (diagnostic, headless only) */
      fprintf(stderr, "[cpu] EL%d pc=0x%llx sp=0x%llx daif=0x%x idle=%d steps=%llu cnt_skip=%llu exceptions=%llu fiq=%llu irq_line=%d esr=0x%llx elr=0x%llx far=0x%llx\n", c->el, (unsigned long long)c->pc, (unsigned long long)c->sp, c->daif, (int)c->idle,
              (unsigned long long)c->steps, (unsigned long long)c->cnt_skip, (unsigned long long)c->exc_count, (unsigned long long)c->fiq_count, (int)c->irq_line, (unsigned long long)c->esr_el1, (unsigned long long)c->elr_el1, (unsigned long long)c->far_el1);
      if (c->hist_pc) { fprintf(stderr, "[hist] entries of pc 0x%llx by (lr, x1): other=%llu\n", (unsigned long long)c->hist_pc, (unsigned long long)c->hist_other); for (unsigned k = 0; k < c->hist_n; k++) fprintf(stderr, "[hist]  lr=%llx x1=%llx n=%llu\n", (unsigned long long)c->hist_lr[k], (unsigned long long)c->hist_x1[k], (unsigned long long)c->hist_cnt[k]); }
      fprintf(stderr, "[cpu] wfi/wfe executed=%llu, of which put the cpu to sleep=%llu\n", (unsigned long long)c->wfi_n, (unsigned long long)c->wfi_slept);
      fprintf(stderr, "[tempsensor] LAST 60 accesses (r/w +offset from the pmgr block start = value):%s\n", a64m_tempsensor_trace());
      { unsigned tot = c->call_total, n = tot < 60 ? tot : 60; fprintf(stderr, "[calls] %u distinct bl targets seen; the %u most recently FIRST-called (newest last):", tot, n); for (unsigned k = tot - n; k < tot; k++) fprintf(stderr, " %llx", (unsigned long long)c->callring[k % 160]); fprintf(stderr, "\n"); }
      for (int k = 0; k < c->exc_first_n; k++) fprintf(stderr, "[exc] sync exception #%d at step %llu: esr=0x%x elr=0x%llx far=0x%llx\n", k, (unsigned long long)c->exc_first[k].steps, c->exc_first[k].esr, (unsigned long long)c->exc_first[k].elr, (unsigned long long)c->exc_first[k].far);
      if (c->exc0_valid) { fprintf(stderr, "[exc] regs at the first sync exception: sp=%llx", (unsigned long long)c->exc0_sp); for (int r = 0; r < 31; r++) fprintf(stderr, " x%d=%llx", r, (unsigned long long)c->exc0_x[r]); fprintf(stderr, "\n"); }
      if (c->trap_note[0]) fprintf(stderr, "[exc] trap note: %s\n", c->trap_note);
      { unsigned n = c->cr_n < 64 ? c->cr_n : 64, st = c->cr_n < 64 ? 0 : c->cr_n % 64; fprintf(stderr, "[bl] last %u bl call sites executed with interrupts unmasked (site->target, newest last):", n); for (unsigned k = 0; k < n; k++) { unsigned qq = (st + k) % 64; fprintf(stderr, " %llx>%llx", (unsigned long long)c->cr_pc[qq], (unsigned long long)c->cr_tg[qq]); } fprintf(stderr, "\n"); }
      { uint64_t fn, site; int dep; fprintf(stderr, "[chain] callers of the code that references '/sbin/launchd' (depth: function start <- call site; X = its first instruction WAS executed):\n");
        for (int k = 0; a64_attempt_chain(k, &fn, &site, &dep); k++) { uint64_t co = fn - c->cov_base; int ex = c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7))); fprintf(stderr, "[chain] %d: %llx <- %llx %s\n", dep, (unsigned long long)fn, (unsigned long long)site, ex ? "X" : "-"); } }
      { uint64_t fn3, site3; int dep3; for (int ck = 0; a64_attempt_chain(ck, &fn3, &site3, &dep3); ck++) { uint64_t st2, tg2; int last = -1, n2 = 0;
          for (int k = 0; a64_attempt_bsdinit(ck, k, &st2, &tg2); k++) { n2++; uint64_t co = st2 - c->cov_base; if (c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7)))) last = k; }
          if (last < 0) continue;
          fprintf(stderr, "[calls-in] chain #%d (depth %d, fn %llx): %d bl sites, last executed #%d; around it (X = executed):", ck, dep3, (unsigned long long)fn3, n2, last);
          for (int k = last - 3 < 0 ? 0 : last - 3; k < n2 && k <= last + 2; k++) { a64_attempt_bsdinit(ck, k, &st2, &tg2); uint64_t co = st2 - c->cov_base; int ex = c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7))); fprintf(stderr, " #%d %llx>%llx%s", k, (unsigned long long)st2, (unsigned long long)tg2, ex ? "X" : "-"); } fprintf(stderr, "\n"); } }
      { /* 0.4.163 diagnostic: reconstruct kernel call chains from frame records [fp, lr] in guest RAM (ASSUMPTIONS: kernel heap/stacks are linearly mapped as VA = 0xffffffe000000000 + (PA - 0x40000000), saved lr values are unsigned in this interpreter) */
        uint64_t sz = 0; for (uint64_t t = 0x1000000ull; t <= 0x100000000ull; t += 0x1000000ull) if (a64m_ram_at(m, 0x40000000ull, t)) sz = t; else break;
        uint8_t *ram = sz ? a64m_ram_at(m, 0x40000000ull, sz) : 0; const uint64_t VB = 0xffffffe000000000ull, lo = c->cov_base, hi = c->cov_base + c->cov_len;
        if (ram) { uint8_t *ref = calloc(sz / 16 / 8 + 1, 1); unsigned nch = 0;
          for (uint64_t p = 0; p + 16 <= sz; p += 16) { uint64_t fp, lr; memcpy(&fp, ram + p, 8); memcpy(&lr, ram + p + 8, 8); if (lr < lo || lr >= hi || fp < VB) continue; uint64_t o2 = fp - VB; if (o2 <= p || o2 - p >= 0x4000 || (o2 & 15) || o2 + 16 > sz) continue; ref[o2 / 16 / 8] |= (uint8_t)(1u << ((o2 / 16) & 7)); }
          for (uint64_t p = 0; p + 16 <= sz && nch < 150; p += 16) { uint64_t fp, lr; memcpy(&fp, ram + p, 8); memcpy(&lr, ram + p + 8, 8); if (lr < lo || lr >= hi || fp < VB || (ref[p / 16 / 8] >> ((p / 16) & 7)) & 1) continue;
            uint64_t q = p; int depth = 0; uint64_t lrs[24]; while (depth < 24) { uint64_t f2, l2; memcpy(&f2, ram + q, 8); memcpy(&l2, ram + q + 8, 8); if (l2 < lo || l2 >= hi) break; lrs[depth++] = l2; if (f2 < VB) break; uint64_t o2 = f2 - VB; if (o2 <= q || o2 - q >= 0x4000 || (o2 & 15) || o2 + 16 > sz) break; q = o2; }
            if (depth < 9) continue; nch++; fprintf(stderr, "[stack] @%llx depth %d:", (unsigned long long)(VB + p), depth); for (int k = 0; k < depth && k < 16; k++) fprintf(stderr, " %llx", (unsigned long long)lrs[k]); fprintf(stderr, "\n"); }
          fprintf(stderr, "[stack] RAM scanned: 0x%llx bytes, %u chains of depth >= 9 printed (max 150)\n", (unsigned long long)sz, nch); free(ref); } }
      fprintf(stderr, "[cpu] last pcs:"); for (unsigned k = 0; k < 24; k++) fprintf(stderr, " 0x%llx", (unsigned long long)c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 24 + k) % A64_PC_HIST]); fprintf(stderr, "\n");
      fprintf(stderr, "[cpu] x0..x8:"); for (int k = 0; k < 9; k++) fprintf(stderr, " %llx", (unsigned long long)c->x[k]); fprintf(stderr, " lr=%llx ttbr0=%llx ttbr1=%llx tpidr_el0=%llx\n", (unsigned long long)c->x[30], (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->tpidr_el0); }
    return 0;
}
