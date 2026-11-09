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
    while ((st = a64m_run(m, 200000, false)) == A64M_RUNNING) { steps += 200000; if (steps % 500000000ull == 0) { A64 *c0 = a64m_cpu(m); fprintf(stderr, "[progress] %llu M steps: EL%d pc=0x%llx daif=0x%x exceptions=%llu x0=%llx lr=%llx\n", (unsigned long long)(steps / 1000000ull), c0->el, (unsigned long long)c0->pc, c0->daif, (unsigned long long)c0->exc_count, (unsigned long long)c0->x[0], (unsigned long long)c0->x[30]); } if (steps > maxm * 1000000ull) { fprintf(stderr, "step limit reached\n"); break; } }
    a64m_log_flush(m);
    size_t off = 0, n; char *b = malloc(65536);
    while ((n = a64m_log_read(m, off, b, 65536)) > 0) { fwrite(b, 1, n, stdout); off += n; }
    fprintf(stderr, "final state %d, log %zu bytes\n", (int)st, off);
    { A64 *c = a64m_cpu(m);   /* 0.4.133: where is the CPU when the run ends? (diagnostic, headless only) */
      fprintf(stderr, "[cpu] EL%d pc=0x%llx sp=0x%llx daif=0x%x idle=%d steps=%llu cnt_skip=%llu exceptions=%llu fiq=%llu irq_line=%d esr=0x%llx elr=0x%llx far=0x%llx\n", c->el, (unsigned long long)c->pc, (unsigned long long)c->sp, c->daif, (int)c->idle,
              (unsigned long long)c->steps, (unsigned long long)c->cnt_skip, (unsigned long long)c->exc_count, (unsigned long long)c->fiq_count, (int)c->irq_line, (unsigned long long)c->esr_el1, (unsigned long long)c->elr_el1, (unsigned long long)c->far_el1);
      fprintf(stderr, "[cpu] last pcs:"); for (unsigned k = 0; k < 24; k++) fprintf(stderr, " 0x%llx", (unsigned long long)c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 24 + k) % A64_PC_HIST]); fprintf(stderr, "\n");
      fprintf(stderr, "[cpu] x0..x8:"); for (int k = 0; k < 9; k++) fprintf(stderr, " %llx", (unsigned long long)c->x[k]); fprintf(stderr, " lr=%llx ttbr0=%llx ttbr1=%llx tpidr_el0=%llx\n", (unsigned long long)c->x[30], (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->tpidr_el0); }
    return 0;
}
