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
      { /* 0.4.173 diagnostic: raw words around the mutex address seen at the frontier (x1 of the last lck_mtx slow-path new pc) */
        uint64_t mv = 0xffffffe000868550ull, pa;
        if (a64_virt_to_phys(c, mv & ~0xfull, &pa) == 0) { uint8_t *q = a64m_ram_at(m, pa, 16); if (q) { uint64_t w0, w1; memcpy(&w0, q, 8); memcpy(&w1, q + 8, 8);
          fprintf(stderr, "[mtx] %llx: %016llx %016llx (TPIDR_EL1=%llx)\n", (unsigned long long)(mv & ~0xfull), (unsigned long long)w0, (unsigned long long)w1, (unsigned long long)c->tpidr_el1); } }
        else fprintf(stderr, "[mtx] %llx not mapped\n", (unsigned long long)mv);
      }
      { /* 0.4.174 diagnostic: raw words of the mutex owner thread struct (0xffffffe000745fe0) and of the current thread */
        uint64_t th[2] = {0xffffffe000745fe0ull, c->tpidr_el1};
        for (int t = 0; t < 2; t++) for (uint64_t off = 0; off < 0x300; off += 0x20) { uint64_t pa; uint8_t *q;
          if (a64_virt_to_phys(c, th[t] + off, &pa) != 0 || !(q = a64m_ram_at(m, pa, 32))) { fprintf(stderr, "[thr%d] +%03llx unmapped\n", t, (unsigned long long)off); continue; }
          uint64_t w[4]; memcpy(w, q, 32);
          fprintf(stderr, "[thr%d] +%03llx: %016llx %016llx %016llx %016llx\n", t, (unsigned long long)off, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]); }
      }
      { /* 0.4.175 diagnostic: raw words around the queue head 0xffffffe000868550 and the static queue 0xfffffff008f94e90 */
        uint64_t bs[3] = {0xffffffe000868500ull, 0xfffffff008f94e40ull, 0xfffffff008f94d00ull};
        for (int t = 0; t < 3; t++) for (uint64_t off = 0; off < 0x100; off += 0x20) { uint64_t pa; uint8_t *q;
          if (a64_virt_to_phys(c, bs[t] + off, &pa) != 0 || !(q = a64m_ram_at(m, pa, 32))) { fprintf(stderr, "[q%d] %llx unmapped\n", t, (unsigned long long)(bs[t] + off)); continue; }
          uint64_t w[4]; memcpy(w, q, 32);
          fprintf(stderr, "[q%d] %llx: %016llx %016llx %016llx %016llx\n", t, (unsigned long long)(bs[t] + off), (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]); }
      }
      { /* 0.4.176 diagnostic: kernel text addresses found on the 16 KB kernel stack of the mutex-owner thread (stack base taken from thread +0x98) */
        uint64_t sb = 0, pa; uint8_t *q;
        if (a64_virt_to_phys(c, 0xffffffe000745fe0ull + 0x98, &pa) == 0 && (q = a64m_ram_at(m, pa, 8))) memcpy(&sb, q, 8);
        fprintf(stderr, "[kstk] owner thread stack base %llx; text words from the top down (addr@offset):", (unsigned long long)sb);
        if (sb) for (uint64_t off = 0x4000; off >= 8; off -= 8) { uint64_t va = sb + off - 8, v = 0;
          if (a64_virt_to_phys(c, va, &pa) != 0 || !(q = a64m_ram_at(m, pa, 8))) continue; memcpy(&v, q, 8);
          uint64_t t = 0xfffffff000000000ull | (v & 0xfffffffffull); if (t >= c->cov_base && t < c->cov_base + c->cov_len && (v >> 36) != 0 && (v & 3) == 0) { uint64_t ppa; uint8_t *iq; uint32_t ins = 0; long long tg = -1; if (a64_virt_to_phys(c, t - 4, &ppa) == 0 && (iq = a64m_ram_at(m, ppa, 4))) { memcpy(&ins, iq, 4); if ((ins >> 26) == 0x25) { int64_t im = (int32_t)(ins << 6) >> 6; tg = (long long)((t - 4 + (uint64_t)(im * 4)) & 0xfffffffffull); } }
            if (tg >= 0) fprintf(stderr, " %llx@%llx(bl %llx)", (unsigned long long)(t & 0xfffffffffull), (unsigned long long)(off - 8), (unsigned long long)tg); else fprintf(stderr, " %llx@%llx", (unsigned long long)(t & 0xfffffffffull), (unsigned long long)(off - 8)); } }
        fprintf(stderr, "\n");
      }
      { /* 0.4.178 diagnostic: strings referenced (ADRP+ADD) in the 0x300 bytes before each return address on the owner thread's kernel stack: names the code that called the blocking wait */
        uint64_t sb = 0, pa; uint8_t *q;
        if (a64_virt_to_phys(c, 0xffffffe000745fe0ull + 0x98, &pa) == 0 && (q = a64m_ram_at(m, pa, 8))) memcpy(&sb, q, 8);
        uint64_t seen[16]; int ns = 0;
        if (sb) for (uint64_t off = 0x4000; off >= 8 && ns < 16; off -= 8) { uint64_t v = 0;
          if (a64_virt_to_phys(c, sb + off - 8, &pa) != 0 || !(q = a64m_ram_at(m, pa, 8))) continue; memcpy(&v, q, 8);
          uint64_t t = 0xfffffff000000000ull | (v & 0xfffffffffull); if (!((v >> 36) != 0 && (v & 3) == 0 && t >= c->cov_base && t < c->cov_base + c->cov_len)) continue;
          int dup = 0; for (int z = 0; z < ns; z++) if (seen[z] == t) dup = 1; if (dup) continue; seen[ns++] = t;
          fprintf(stderr, "[kstr] ret %llx:", (unsigned long long)(t & 0xfffffffffull));
          int cnt = 0; for (uint64_t a = t - 0x300; a < t && cnt < 4; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff) ;
            uint64_t sp; uint8_t *sq; char buf[48]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 40))) continue; memcpy(buf, sq, 40); buf[40] = 0;
            int ok = 1, n = 0; while (n < 40 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 5) continue;
            fprintf(stderr, " \"%s\"", buf); cnt++; }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.179 diagnostic: strings referenced (ADRP+ADD) in code ranges around the last executed bl of the '/sbin/launchd' chain function (fn+0x8e4 area) and in its callee */
        static const uint64_t rg[3][2] = {{0xfffffff008e7e7c0ull, 0xfffffff008e7e930ull}, {0xfffffff007c1fa74ull, 0xfffffff007c1fc74ull}, {0xfffffff007bc541cull, 0xfffffff007bc561cull}};
        for (int r = 0; r < 3; r++) { fprintf(stderr, "[rstr] %llx..%llx:", (unsigned long long)(rg[r][0] & 0xfffffffffull), (unsigned long long)(rg[r][1] & 0xfffffffffull));
          for (uint64_t a = rg[r][0]; a < rg[r][1]; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
            int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
            fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.180 diagnostic: strings referenced (ADRP+ADD, 0x400 bytes back) before return addresses of the blocked thread whose stack chain is 7a68fcc 8151cbc 80eaa68 7e8e3a8 7a75284 */
        static const uint64_t rets[5] = {0xfffffff008151cbcull, 0xfffffff0080eaa68ull, 0xfffffff007e8e3a8ull, 0xfffffff007a75284ull, 0xfffffff007a68fccull};
        for (int r = 0; r < 5; r++) { fprintf(stderr, "[bstr] ret %llx:", (unsigned long long)(rets[r] & 0xfffffffffull)); int cnt = 0;
          for (uint64_t a = rets[r] - 0x400; a < rets[r] && cnt < 6; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
            int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
            fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); cnt++; }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.181 diagnostic: bl target at each return address of the bsd-init thread chain (IOFindBSDRoot -> ... -> thread_block) and strings referenced in the first 0x300 bytes of that target */
        static const uint64_t rets[4] = {0xfffffff0080eaa68ull, 0xfffffff008151cbcull, 0xfffffff007e8e3a8ull, 0xfffffff007a75284ull};
        for (int r = 0; r < 4; r++) { uint64_t pp; uint8_t *iq; uint32_t ins = 0; if (a64_virt_to_phys(c, rets[r] - 4, &pp) || !(iq = a64m_ram_at(m, pp, 4))) continue; memcpy(&ins, iq, 4);
          if ((ins >> 26) != 0x25) { fprintf(stderr, "[btgt] ret %llx: not a bl (%08x)\n", (unsigned long long)(rets[r] & 0xfffffffffull), ins); continue; }
          uint64_t tgt = rets[r] - 4 + (uint64_t)(((int64_t)(int32_t)(ins << 6) >> 6) * 4);
          fprintf(stderr, "[btgt] ret %llx -> bl %llx:", (unsigned long long)(rets[r] & 0xfffffffffull), (unsigned long long)(tgt & 0xfffffffffull)); int cnt = 0;
          for (uint64_t a = tgt; a < tgt + 0x300 && cnt < 6; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
            int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
            fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); cnt++; }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.182 diagnostic: strings (all) and bl targets in 0x8151a80..0x8151cc0, the function that calls the blocking helper (7a6c3a8) before thread_block */
        fprintf(stderr, "[wstr] 8151a80..8151cc0:");
        for (uint64_t a = 0xfffffff008151a80ull; a < 0xfffffff008151cc0ull; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
          if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4);
          if ((i1 >> 26) == 0x25) fprintf(stderr, " @%llx:bl %llx", (unsigned long long)(a & 0xfffffffffull), (unsigned long long)((a + (uint64_t)(((int64_t)(int32_t)(i1 << 6) >> 6) * 4)) & 0xfffffffffull));
          if (a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i2, r2, 4);
          if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
          int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
          uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
          int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 3) continue;
          fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); }
        fprintf(stderr, "\n");
      }
      { /* 0.4.184 diagnostic: every ADRP+ADD reference in kernel code to the strings "SecureRoot" and "IORTC" (the platform method waits for flags set by a matching callback for these), with nearby strings, to find who publishes them */
        static const char *want[3] = {"SecureRoot", "IORTC", "SecureRootCallBack"}; static const uint64_t hint[3] = {0xfffffff008151b58ull, 0xfffffff008151ae0ull, 0xfffffff008151d54ull};
        for (int w = 2; w < 3; w++) { uint64_t strva = 0; uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
          { uint64_t a = hint[w] - 8; if (!(a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4)))) { memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
              int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; strva = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff); } }
          /* the hint address is only approximate: scan +-0x20 for the exact ADRP+ADD whose target string equals want[w] */
          strva = 0; for (uint64_t a = hint[w] - 0x30; a < hint[w] + 0x30 && !strva; a += 4) { if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[32]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 16))) continue; memcpy(buf, sq, 16); buf[15] = 0; if (!strcmp(buf, want[w])) strva = tg; }
          fprintf(stderr, "[sref] \"%s\" string at %llx; references:", want[w], (unsigned long long)(strva & 0xfffffffffull)); if (!strva) { fprintf(stderr, " (string not located)\n"); continue; }
          int nref = 0; for (uint64_t a = c->cov_base; a < c->cov_base + c->cov_len && nref < 12; a += 4) { if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4); if ((i1 & 0x9f000000u) != 0x90000000u) continue;
            if (a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i2, r2, 4); if ((i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff); if (tg != strva) continue;
            nref++; fprintf(stderr, " @%llx{", (unsigned long long)(a & 0xfffffffffull)); int cnt = 0;
            for (uint64_t b = a - 0x180; b < a + 0x180 && cnt < 5; b += 4) { uint32_t j1 = 0, j2 = 0; uint64_t q1, q2; uint8_t *s1, *s2; if (b == a) continue;
              if (a64_virt_to_phys(c, b, &q1) || !(s1 = a64m_ram_at(m, q1, 4)) || a64_virt_to_phys(c, b + 4, &q2) || !(s2 = a64m_ram_at(m, q2, 4))) continue; memcpy(&j1, s1, 4); memcpy(&j2, s2, 4);
              if ((j1 & 0x9f000000u) != 0x90000000u || (j2 & 0xff800000u) != 0x91000000u || ((j2 >> 5) & 31) != (j1 & 31)) continue;
              int64_t jm = ((int64_t)(((j1 >> 5) & 0x7ffff) << 2 | ((j1 >> 29) & 3)) << 43) >> 43; uint64_t jt = ((b & ~0xfffull) + (uint64_t)(jm << 12)) + ((j2 >> 10) & 0xfff);
              uint64_t sp; uint8_t *sq; char bf[48]; if (a64_virt_to_phys(c, jt, &sp) || !(sq = a64m_ram_at(m, sp, 44))) continue; memcpy(bf, sq, 44); bf[44] = 0; int ok = 1, n = 0; while (n < 44 && bf[n]) { if ((unsigned char)bf[n] < 32 || (unsigned char)bf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
              fprintf(stderr, "%s\"%s\"", cnt ? " " : "", bf); cnt++; }
            fprintf(stderr, "}"); }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.186 diagnostic: walk the frame records from the bsd-init thread (slot ffffffe0107c3910); in the frame whose saved lr is 0x80eaa68 (the AppleARMPE callPlatformFunction method) print the saved x19..x24 (this = x19) and the platform object words +0x100..+0x120 (flags +0x108 no-rtc, +0x109 no-suspend, +0x10a request, +0x10b answer, +0x10c result, +0x110 provider) */
        uint64_t cur = 0xffffffe0107c3910ull;
        for (int d = 0; d < 8; d++) { uint64_t pa, fp = 0, lr = 0; uint8_t *q;
          if (a64_virt_to_phys(c, cur, &pa) || !(q = a64m_ram_at(m, pa, 16))) break; memcpy(&fp, q, 8); memcpy(&lr, q + 8, 8);
          fprintf(stderr, "[fr] #%d slot %llx fp %llx lr %llx\n", d, (unsigned long long)cur, (unsigned long long)fp, (unsigned long long)(lr & 0xfffffffffull));
          if ((lr & 0xfffffffffull) == 0x80eaa68ull) { uint64_t sv[7]; for (int z = 0; z < 7; z++) { sv[z] = 0; uint64_t pp; uint8_t *qq; if (!a64_virt_to_phys(c, cur - 0x38 + 8 * z, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&sv[z], qq, 8); }
            fprintf(stderr, "[fr] saved (slot-0x38..slot-8): x24=%llx x23=%llx x22=%llx x21=%llx x20=%llx x19=%llx\n", (unsigned long long)sv[0], (unsigned long long)sv[1], (unsigned long long)sv[2], (unsigned long long)sv[3], (unsigned long long)sv[4], (unsigned long long)sv[5]);
            uint64_t obj = sv[5]; for (uint64_t off = 0x100; off < 0x120; off += 8) { uint64_t pp, w = 0; uint8_t *qq; if (!a64_virt_to_phys(c, obj + off, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&w, qq, 8); fprintf(stderr, "[plat] +%llx = %016llx\n", (unsigned long long)off, (unsigned long long)w); }
            break; }
          if (!fp) break; cur = fp; }
      }
      { /* 0.4.187 diagnostic: raw stack words around the AppleARMPE method frame (slot ffffffe0107c39b0), and for every word that looks like a kernel pointer the 8 bytes at +0x108 and +0x110 with a mapped flag */
        const uint64_t slot = 0xffffffe0107c39b0ull;
        for (int z = -10; z <= 1; z++) { uint64_t va = slot + 8 * (int64_t)z, pa, w = 0; uint8_t *q; int ok = !a64_virt_to_phys(c, va, &pa) && (q = a64m_ram_at(m, pa, 8)); if (ok) memcpy(&w, q, 8);
          fprintf(stderr, "[sf] slot%+d: %016llx", z * 8, (unsigned long long)w);
          if (ok && (w >> 36) == 0xfffffffull || ok && (w >> 36) == 0xfffffffffull) { for (int r = 0; r < 2; r++) { uint64_t o = w + (r ? 0x110 : 0x108), p2, v = 0; uint8_t *q2; int ok2 = !a64_virt_to_phys(c, o, &p2) && (q2 = a64m_ram_at(m, p2, 8)); if (ok2) memcpy(&v, q2, 8); fprintf(stderr, "  [+%x]=%s%016llx", r ? 0x110 : 0x108, ok2 ? "" : "UNMAPPED:", (unsigned long long)v); } }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.188 diagnostic: the real "this" of the AppleARMPE method is the heap object saved at slot-8 (ffffffe0004b56a0); dump +0..+0x10 and +0x100..+0x120 */
        const uint64_t obj = 0xffffffe0004b56a0ull;
        for (uint64_t off = 0; off < 0x130; off += 8) { if (off == 0x10) off = 0x100; uint64_t pa, w = 0; uint8_t *q; int ok = !a64_virt_to_phys(c, obj + off, &pa) && (q = a64m_ram_at(m, pa, 8)); if (ok) memcpy(&w, q, 8);
          fprintf(stderr, "[this] +%03llx = %s%016llx\n", (unsigned long long)off, ok ? "" : "UNMAPPED:", (unsigned long long)w); }
      }
      { /* 0.4.189 diagnostic: every STRB w,[x,#0x10b] (answer flag writer) and STRB w,[x,#0x10a] in kernel code; [this] 0.4.188: flags are +0x10a=1 (request), +0x10b=0 (answer missing), +0x10c=1 */
        fprintf(stderr, "[wr10b] strb +0x10b at:"); int n1 = 0; for (uint64_t a = c->cov_base; a < c->cov_base + c->cov_len && n1 < 24; a += 4) { uint64_t pa; uint8_t *q; uint32_t i; if (a64_virt_to_phys(c, a, &pa) || !(q = a64m_ram_at(m, pa, 4))) continue; memcpy(&i, q, 4); if ((i & 0xFFFFFC00u) == 0x39042C00u) { fprintf(stderr, " %llx", (unsigned long long)(a & 0xfffffffffull)); n1++; } }
        fprintf(stderr, "\n[wr10b] strb +0x10a at:"); n1 = 0; for (uint64_t a = c->cov_base; a < c->cov_base + c->cov_len && n1 < 24; a += 4) { uint64_t pa; uint8_t *q; uint32_t i; if (a64_virt_to_phys(c, a, &pa) || !(q = a64m_ram_at(m, pa, 4))) continue; memcpy(&i, q, 4); if ((i & 0xFFFFFC00u) == 0x39042800u) { fprintf(stderr, " %llx", (unsigned long long)(a & 0xfffffffffull)); n1++; } }
        fprintf(stderr, "\n");
      }
      { /* 0.4.167 diagnostic: kernel call chains from frame records [fp, lr] found by scanning the kernel heap VA range page by page through the guest MMU (the heap is NOT linearly mapped, measured in 0.4.166). ASSUMPTION: saved lr may carry a PAC signature, only its low 36 bits are used */
        const uint64_t lo = c->cov_base, hi = c->cov_base + c->cov_len; const uint64_t VS = 0xffffffe000000000ull, VE = VS + 0x20000000ull;
        static uint64_t vis[1 << 20]; unsigned nch = 0, npg = 0, nuq = 0; static char uqs[400][420]; static unsigned uqn[400]; static uint64_t uqs_slot[400];
        for (uint64_t va = VS; va < VE && nch < 5000; va += 0x1000) { uint64_t pa; if (a64_virt_to_phys(c, va, &pa) != 0) continue; uint8_t *pg = a64m_ram_at(m, pa & ~0xfffull, 0x1000); if (!pg) continue; npg++;
          for (uint64_t o = 0; o < 0x1000 && nch < 5000; o += 16) { uint64_t fp, lr; memcpy(&fp, pg + o, 8); memcpy(&lr, pg + o + 8, 8); lr = 0xfffffff000000000ull | (lr & 0xfffffffffull); uint64_t slot = va + o;
            if (lr < lo || lr >= hi || fp <= slot || fp - slot >= 0x4000 || (fp & 15)) continue; { unsigned h = (unsigned)((slot >> 4) * 0x9e3779b1u) & ((1u << 20) - 1); int seen = 0; for (int pr = 0; pr < 8; pr++) { if (vis[(h + pr) & ((1u << 20) - 1)] == slot) { seen = 1; break; } } if (seen) continue; }
            uint64_t cur = slot; int depth = 0; uint64_t lrs[24]; while (depth < 24) { uint64_t pa2, f2, l2; if (a64_virt_to_phys(c, cur, &pa2) != 0) break; uint8_t *q2 = a64m_ram_at(m, pa2, 16); if (!q2) break; memcpy(&f2, q2, 8); memcpy(&l2, q2 + 8, 8); l2 = 0xfffffff000000000ull | (l2 & 0xfffffffffull); if (l2 < lo || l2 >= hi) break;
              { unsigned h = (unsigned)((cur >> 4) * 0x9e3779b1u) & ((1u << 20) - 1); for (int pr = 0; pr < 8; pr++) if (!vis[(h + pr) & ((1u << 20) - 1)]) { vis[(h + pr) & ((1u << 20) - 1)] = cur; break; } }
              lrs[depth++] = l2; if (f2 <= cur || f2 - cur >= 0x4000 || (f2 & 15)) break; cur = f2; }
            if (depth < 6) continue; nch++; { char sg[420]; int sn2 = 0; sg[0] = 0; for (int k = 0; k < depth && k < 16 && sn2 < 400; k++) sn2 += snprintf(sg + sn2, sizeof sg - (size_t)sn2, " %llx", (unsigned long long)(lrs[k] & 0xfffffffffull)); int f = -1; for (unsigned u = 0; u < nuq; u++) if (!strcmp(uqs[u], sg)) { f = (int)u; break; } if (f >= 0) uqn[f]++; else if (nuq < 400) { strcpy(uqs[nuq], sg); uqn[nuq] = 1; uqs_slot[nuq++] = slot; } } } }
        for (unsigned u = 0; u < nuq; u++) { fprintf(stderr, "[stack] x%u @%llx:", uqn[u], (unsigned long long)uqs_slot[u]); char *tk = uqs[u]; unsigned long long av; int cn = 0; while (*tk && cn < 16) { while (*tk == ' ') tk++; if (!*tk) break; av = strtoull(tk, &tk, 16); uint64_t so; const char *sn = a64_attempt_symbolize(0xfffffff000000000ull | av, &so); if (sn) fprintf(stderr, " %s+%llx", sn, (unsigned long long)so); else fprintf(stderr, " %llx", av); cn++; } fprintf(stderr, "\n"); } fprintf(stderr, "[stack] mapped heap pages scanned: %u, %u chains of depth >= 6, %u distinct (low 36 bits of the addresses, 0xfffffff000000000 prefix omitted)\n", npg, nch, nuq); }
      { unsigned n = c->nw_n < 96 ? c->nw_n : 96, st = c->nw_n < 96 ? 0 : c->nw_n % 96; fprintf(stderr, "[new] %u instructions executed for the first time in total; the last %u (pc@step/1e6, newest last):", c->nw_n, n); for (unsigned k = 0; k < n; k++) { unsigned qq = (st + k) % 96; fprintf(stderr, " %llx@%llu", (unsigned long long)c->nw_pc[qq], (unsigned long long)(c->nw_step[qq] / 1000000)); } fprintf(stderr, "\n"); for (unsigned k = n > 6 ? n - 6 : 0; k < n; k++) { unsigned qq = (st + k) % 96; fprintf(stderr, "[new] regs at new pc %llx: x0=%llx x1=%llx lr=%llx\n", (unsigned long long)c->nw_pc[qq], (unsigned long long)c->nw_x0[qq], (unsigned long long)c->nw_x1[qq], (unsigned long long)c->nw_lr[qq]); } }
      fprintf(stderr, "[cpu] last pcs:"); for (unsigned k = 0; k < 24; k++) fprintf(stderr, " 0x%llx", (unsigned long long)c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 24 + k) % A64_PC_HIST]); fprintf(stderr, "\n");
      fprintf(stderr, "[cpu] x0..x8:"); for (int k = 0; k < 9; k++) fprintf(stderr, " %llx", (unsigned long long)c->x[k]); fprintf(stderr, " lr=%llx ttbr0=%llx ttbr1=%llx tpidr_el0=%llx\n", (unsigned long long)c->x[30], (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->tpidr_el0); }
    return 0;
}
