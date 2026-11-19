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
    { const char *hp = getenv("XN_HIST_PC"); if (hp) a64m_cpu(m)->hist_pc = strtoull(hp, 0, 16); } a64m_cpu(m)->cr_on = 1; a64m_cpu(m)->snap_pc = 0xfffffff007a44164ull;   /* 0.4.138 diagnostic; 0.4.212: snapshot at the first entry of panic() */
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
        static const uint64_t rets[14] = {0xfffffff008836fccull, 0xfffffff00814a9f0ull, 0xfffffff00814a91cull, 0xfffffff0080aaa20ull, 0xfffffff00883bd2cull, 0xfffffff008b46438ull, 0xfffffff0082db9e8ull, 0xfffffff00809d0ccull, 0xfffffff0081613c0ull, 0xfffffff008075e84ull, 0xfffffff0080a2b1cull, 0xfffffff0080a1ea0ull, 0xfffffff008831b78ull, 0xfffffff0083100d0ull};
        for (int r = 0; r < 14; r++) { fprintf(stderr, "[bstr] ret %llx:", (unsigned long long)(rets[r] & 0xfffffffffull)); int cnt = 0;
          for (uint64_t a = rets[r] - 0x400; a < rets[r] && cnt < 8; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
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
            uint64_t sp; uint8_t *sq; char buf[40]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 32))) continue; memcpy(buf, sq, 32); buf[31] = 0; if (!strcmp(buf, want[w])) strva = tg; }
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
      { /* 0.4.193 diagnostic: find "_sep_enabled" in the kernel cstring area, then every ADRP+ADD reference to its string; print the code words from -0x1c0 to +0x40 around each (decides how AppleSEPKeyStore::start computes _sep_enabled and whether it waits for the SEP) */
        uint64_t strva = 0; static const char nd[] = "_sep_enabled";
        for (uint64_t pg = 0xfffffff007400000ull; pg < 0xfffffff007700000ull && !strva; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
          for (unsigned o = 0; o + sizeof nd < 0x1000; o++) if (!memcmp(q + o, nd, sizeof nd - 1)) { unsigned st = o; while (st > 0 && q[st - 1]) st--; strva = pg + st; break; } }
        fprintf(stderr, "[sep] string at %llx\n", (unsigned long long)(strva & 0xfffffffffull));
        int nref = 0; if (strva) for (uint64_t a = c->cov_base; a < c->cov_base + c->cov_len && nref < 3; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
          if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4); if ((i1 & 0x9f000000u) != 0x90000000u) continue;
          if (a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i2, r2, 4); if ((i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
          int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff); if (tg != strva) continue;
          nref++; fprintf(stderr, "[sep] ref at %llx; code from %llx:", (unsigned long long)(a & 0xfffffffffull), (unsigned long long)((a - 0x1c0) & 0xfffffffffull));
          for (uint64_t b = a - 0x1c0; b < a + 0x40; b += 4) { uint32_t w = 0; uint64_t pp; uint8_t *qq; if (!a64_virt_to_phys(c, b, &pp) && (qq = a64m_ram_at(m, pp, 4))) memcpy(&w, qq, 4); fprintf(stderr, " %08x", w); }
          fprintf(stderr, "\n"); }
      }
      { /* 0.4.196 diagnostic: which unknown SoC blocks the kernel touched (name reads/writes), to see whether the SEP mailbox block 'sep' (0x242400000) was ever accessed */
        static char us[6000]; int un = a64m_unknown_summary(m, us, sizeof us); fprintf(stderr, "[unk] %d touched blocks:%s\n", un, us);
      }
      { /* 0.4.197 diagnostic: did the kernel ever run SEP-manager code? Find cstrings starting with "AppleSEP" or "SEP" in the cstring area, then every ADRP+ADD reference to them, and say whether that referencing instruction WAS executed (coverage bitmap) */
        static uint64_t sv[600]; static char sn[600][60]; int ns = 0;
        for (uint64_t pg = 0xfffffff007400000ull; pg < 0xfffffff007700000ull && ns < 600; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
          for (unsigned o = 1; o + 20 < 0x1000 && ns < 600; o++) if (!q[o - 1] && ({ char t[60]; memcpy(t, q + o, 59); t[59] = 0; strstr(t, "AppleS8000AES") != NULL || strstr(t, "AES") != NULL; })   /* 0.4.221: needle = tunables (PCIe PHY) */) { sv[ns] = pg + o; snprintf(sn[ns], 60, "%.59s", (const char *)(q + o)); ns++; } }
        fprintf(stderr, "[sepx] %d tunable-related cstrings; referencing code (E = executed, - = never executed):\n", ns);
        for (uint64_t a = c->cov_base; a < c->cov_base + c->cov_len; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
          if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4); if ((i1 & 0x9f000000u) != 0x90000000u) continue;
          if (a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i2, r2, 4); if ((i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
          int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
          for (int z = 0; z < ns; z++) if (sv[z] == tg) { uint64_t co = a - c->cov_base; int ex = c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7)));
            fprintf(stderr, "[sepx]  %c @%llx \"%s\"\n", ex ? 'E' : '-', (unsigned long long)(a & 0xfffffffffull), sn[z]); } }
      }
      { /* 0.4.201 diagnostic: was the code that references "SecureRoot"/"SecureRootCallBack" executed? (instruction addresses of those references; E = executed) */
        static const uint64_t pcs[] = {0xfffffff0083d23f0ull, 0xfffffff0083d272cull, 0xfffffff0089876e4ull};
        (void)pcs;
        static const uint64_t pc2[] = {0xfffffff0083d23f0ull, 0xfffffff0083d272cull, 0xfffffff0089876e4ull, 0xfffffff00898af7cull, 0xfffffff0088b2f50ull, 0xfffffff008151b58ull, 0xfffffff0083d2124ull, 0xfffffff0083d2160ull, 0xfffffff0083d21c4ull, 0xfffffff0083d2210ull, 0xfffffff0083d2214ull, 0xfffffff0083d22f8ull, 0xfffffff0083d23d0ull, 0xfffffff0083d23f8ull, 0xfffffff0083d26e8ull}; /* 0.4.289: + entry points/stages of the keystore registrar 0x83d21c4 */
        fprintf(stderr, "[skx] reference executed? (E yes / - no):");
        for (unsigned z = 0; z < sizeof pc2 / sizeof pc2[0]; z++) { uint64_t co = pc2[z] - c->cov_base; int ex = c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7))); fprintf(stderr, " %c@%llx", ex ? 'E' : '-', (unsigned long long)(pc2[z] & 0xfffffffffull)); }
        fprintf(stderr, "\n");
      }
      { /* 0.4.202 diagnostic: executed/never-executed instruction ranges of AppleSEPKeyStore::start (code around 0x898ac48 where "_sep_enabled" is printed) - where does start() stop? */
        const uint64_t lo = 0xfffffff008075e40ull, hi = 0xfffffff008076200ull; int run = -1; uint64_t rs = lo;
        fprintf(stderr, "[ksx] executed ranges (E) in %llx..%llx:", (unsigned long long)(lo & 0xfffffffffull), (unsigned long long)(hi & 0xfffffffffull));
        for (uint64_t a = lo; a <= hi; a += 4) { uint64_t co = a - c->cov_base; int ex = a < hi && c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7)));
          if (ex != run) { if (run == 1) fprintf(stderr, " %llx-%llx", (unsigned long long)(rs & 0xfffffffffull), (unsigned long long)((a - 4) & 0xfffffffffull)); run = ex; rs = a; } }
        fprintf(stderr, "\n");
      }
      { /* 0.4.320 diagnostic: kernel stacks of BLOCKED threads. Scan 16 KiB-aligned stacks in 0xffffffe01a000000..0xffffffe01c000000 for the longest frame-pointer chain (fp,lr pairs, lr in kernel text after stripping PAC) and print its return addresses (low 36 bits). A blocked thread's live frames are the tail of the chain; dead frames below them may be shown first. */
        A64 *c7 = a64m_cpu(m); int shown = 0; static uint64_t ul[600]; int nul = 0;
        static uint64_t th[0x300 / 8 * 2]; int thok = 0; { uint64_t tv = c7->el0r.tp1; thok = tv != 0; for (uint64_t o = 0; o < 0x600 && thok; o += 8) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c7, tv + o, &pa) || !(q = a64m_ram_at(m, pa, 8))) { thok = 0; break; } memcpy(&th[o / 8], q, 8); } }   /* 0.4.400: the thread of the last EL0 abort: which kernel stack does its struct point to? */
        fprintf(stderr, "[thx] thread struct at 0x%llx readable=%d:", (unsigned long long)c7->el0r.tp1, thok); for (int u = 0; u < 0x600 / 8 && thok; u++) { if (th[u]) fprintf(stderr, " +%x=%llx", u * 8, (unsigned long long)th[u]); } fprintf(stderr, "\n");
        { /* 0.4.427: the global lock AppleA7IOP::start sleeps on (code 0x80bc428: x19=[0xfffffff0091ba7c8], event/arg [..7d0]). Owner = lock word. */
          uint64_t gv[2] = { 0xfffffff0091ba7c8ull, 0xfffffff0091ba7d0ull }; uint64_t gl[2] = { 0, 0 };
          for (int u = 0; u < 2; u++) { uint64_t pa; uint8_t *q; if (!a64_virt_to_phys(c7, gv[u], &pa) && (q = a64m_ram_at(m, pa, 8))) memcpy(&gl[u], q, 8); }
          fprintf(stderr, "[lock] [%llx]=%llx [%llx]=%llx", (unsigned long long)gv[0], (unsigned long long)gl[0], (unsigned long long)gv[1], (unsigned long long)gl[1]);
          uint64_t lw[4] = { 0, 0, 0, 0 }; for (int u = 0; u < 4 && gl[0]; u++) { uint64_t pa; uint8_t *q; if (!a64_virt_to_phys(c7, gl[0] + 8 * (uint64_t)u, &pa) && (q = a64m_ram_at(m, pa, 8))) memcpy(&lw[u], q, 8); }
          fprintf(stderr, " lock words: %llx %llx %llx %llx\n", (unsigned long long)lw[0], (unsigned long long)lw[1], (unsigned long long)lw[2], (unsigned long long)lw[3]);
          uint64_t ow = lw[0]; static uint64_t ot[0x300 / 8]; int ook = ow >= 0xffffffe000000000ull;
          for (uint64_t o = 0; o < 0x300 && ook; o += 8) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c7, ow + o, &pa) || !(q = a64m_ram_at(m, pa, 8))) { ook = 0; break; } memcpy(&ot[o / 8], q, 8); }
          fprintf(stderr, "[lock-owner] thread %llx readable=%d:", (unsigned long long)ow, ook); for (int u = 0; u < 0x300 / 8 && ook; u++) if (ot[u]) fprintf(stderr, " +%x=%llx", u * 8, (unsigned long long)ot[u]); fprintf(stderr, "\n"); }
        { /* 0.4.429: arguments of every (first 16) call of the waitForService-like function 0x80bc408: print x1 (matching object) as a heuristic 3-level pointer tree with printable strings */
          fprintf(stderr, "[ionvram-standin] ch_pc=%llx fired=%u (MODELLED one-shot publishResource IONVRAM)\n", (unsigned long long)c7->ch_pc, c7->ch_n);
          fprintf(stderr, "[wait-calls] %u calls\n", c7->wp_n);
          fprintf(stderr, "[wait-8075e84-snapshot] (first call with lr=0x8075e84):%s\n", c7->wp_snap2_done ? c7->wp_snap2 : " none");
          fprintf(stderr, "[wait-pmgr-snapshot] (x0 of the first call with lr=0x814a9f0, strings up to 4 levels):%s\n", c7->wp_snap_done ? c7->wp_snap : " none");
          for (unsigned kk = c7->wp_n > 16 ? c7->wp_n - 16 : 0; kk < c7->wp_n; kk++) { unsigned k = kk & 15;
            fprintf(stderr, "[wait%u] x0=%llx x1=%llx lr=%llx:", k, (unsigned long long)c7->wp_x0[k], (unsigned long long)c7->wp_x1[k], (unsigned long long)c7->wp_lr[k] & 0xfffffffffull);
            uint64_t root = c7->wp_x0[k]; uint64_t l1[8], l2[8], l3[8];
            for (int i = 0; i < 8; i++) { l1[i] = 0; } { uint64_t pa; uint8_t *q; for (int i = 0; i < 8; i++) if (!a64_virt_to_phys(c7, root + 8 * (uint64_t)i, &pa) && (q = a64m_ram_at(m, pa, 8))) memcpy(&l1[i], q, 8); }
            for (int i = 1; i < 8; i++) { uint64_t p1 = l1[i]; if (p1 < 0xffffffe000000000ull) continue;
              uint64_t pa; uint8_t *q; char sb2[48]; int ok = 0; memset(sb2, 0, sizeof sb2);
              if (!a64_virt_to_phys(c7, p1, &pa) && (q = a64m_ram_at(m, pa, 40))) { memcpy(sb2, q, 40); ok = 1; for (int t = 0; t < 4; t++) if ((unsigned char)sb2[t] < 32 || (unsigned char)sb2[t] > 126) ok = 0; }
              if (ok) { fprintf(stderr, " [%d]=\"%.40s\"", i, sb2); continue; }
              for (int j = 0; j < 8; j++) l2[j] = 0; for (int j = 0; j < 8; j++) if (!a64_virt_to_phys(c7, p1 + 8 * (uint64_t)j, &pa) && (q = a64m_ram_at(m, pa, 8))) memcpy(&l2[j], q, 8);
              fprintf(stderr, " [%d]->{", i);
              for (int j = 0; j < 8; j++) { uint64_t p2 = l2[j]; if (p2 < 0xffffffe000000000ull) continue;
                char s3[48]; memset(s3, 0, sizeof s3); int ok3 = 0; if (!a64_virt_to_phys(c7, p2, &pa) && (q = a64m_ram_at(m, pa, 40))) { memcpy(s3, q, 40); ok3 = 1; for (int t = 0; t < 4; t++) if ((unsigned char)s3[t] < 32 || (unsigned char)s3[t] > 126) ok3 = 0; }
                if (ok3) { fprintf(stderr, "%d:\"%.40s\" ", j, s3); continue; }
                for (int w = 0; w < 8; w++) l3[w] = 0; for (int w = 0; w < 8; w++) if (!a64_virt_to_phys(c7, p2 + 8 * (uint64_t)w, &pa) && (q = a64m_ram_at(m, pa, 8))) memcpy(&l3[w], q, 8);
                fprintf(stderr, "%d:(", j);
                for (int w = 1; w < 8; w++) { uint64_t p3 = l3[w]; if (p3 < 0xffffffe000000000ull) continue; char s4[48]; memset(s4, 0, sizeof s4); if (!a64_virt_to_phys(c7, p3, &pa) && (q = a64m_ram_at(m, pa, 40))) { memcpy(s4, q, 40); int ok4 = 1; for (int t = 0; t < 3; t++) if ((unsigned char)s4[t] < 32 || (unsigned char)s4[t] > 126) ok4 = 0; if (ok4) fprintf(stderr, "%d:\"%.40s\" ", w, s4); } }
                fprintf(stderr, ") "); }
              fprintf(stderr, "}"); }
            fprintf(stderr, "\n"); } }
        for (uint64_t sb = 0xffffffe000000000ull; sb < 0xffffffe040000000ull && shown < 90; sb += 0x4000)   /* 0.4.346: scan from 0xffffffe000000000 (was 1a000000): the first (bootstrap) thread stack may be allocated lower */ { uint64_t pa7; uint8_t *q = NULL; uint8_t stk[0x4000]; int ok = 1;
          for (uint64_t o = 0; o < 0x4000 && ok; o += 0x1000) { if (a64_virt_to_phys(c7, sb + o, &pa7) || !(q = a64m_ram_at(m, pa7, 0x1000))) ok = 0; else memcpy(stk + o, q, 0x1000); }
          if (!ok) continue;
          int best = 0; uint64_t bestA = 0;
          for (uint64_t A = 0; A + 16 <= 0x4000; A += 16) { uint64_t cur = A; int d = 0;
            while (d < 40) { uint64_t fp, lr; memcpy(&fp, stk + cur, 8); memcpy(&lr, stk + cur + 8, 8); uint64_t l36 = lr & 0xfffffffffull; if (l36 < 0x007000000ull || l36 > 0x00a800000ull) break; d++;
              if (fp == 0) break; uint64_t nx = fp - sb; if (fp < sb || nx >= 0x4000 || (nx & 15) || nx <= cur) break; cur = nx; }
            if (d > best) { best = d; bestA = A; } }
          if (best >= 3) { shown++; { int mine = 0; if (thok) for (int u = 0; u < 0x600 / 8; u++) if (th[u] >= sb && th[u] < sb + 0x4000) mine = 1; if (mine) fprintf(stderr, "[stk-last-el0-thread] "); }
          fprintf(stderr, "[stk] %llx depth %d from +%llx:", (unsigned long long)(sb & 0xfffffffffull), best, (unsigned long long)bestA); uint64_t cur = bestA;
            for (int d = 0; d < best; d++) { uint64_t fp, lr; memcpy(&fp, stk + cur, 8); memcpy(&lr, stk + cur + 8, 8); fprintf(stderr, " %llx", (unsigned long long)(lr & 0xfffffffffull)); { int f = 0; for (int u = 0; u < nul; u++) if (ul[u] == (lr & 0xfffffffffull)) f = 1; if (!f && nul < 600) ul[nul++] = lr & 0xfffffffffull; } if (!fp) break; cur = fp - sb; }
            fprintf(stderr, "\n"); } }
        { /* 0.4.368 diagnostic: [e6] scan found no direct stores of 6 into [bp+84]. Find (1) 'str w1,[x0,#84]' (buf_seterror-like setter) and (2) 'mov w1,#6' followed within 3 instructions by a BL whose target contains such a str within 0x30 bytes of its start */
          int nh = 0; uint64_t setter[8]; int ns = 0;
          for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff00a800000ull; pg += 0x1000) { uint64_t pa8; uint8_t *q8; if (a64_virt_to_phys(c7, pg, &pa8) || !(q8 = a64m_ram_at(m, pa8, 0x1000))) continue;
            for (uint32_t o = 0; o < 0x1000; o += 4) { uint32_t w; memcpy(&w, q8 + o, 4); if (w == 0xb9005401u && ns < 8) { setter[ns++] = pg + o; fprintf(stderr, "[e6s] str w1,[x0,#84] at %llx\n", (unsigned long long)((pg + o) & 0xfffffffffull)); } } }
          for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff00a800000ull && nh < 80; pg += 0x1000) { uint64_t pa8; uint8_t *q8; if (a64_virt_to_phys(c7, pg, &pa8) || !(q8 = a64m_ram_at(m, pa8, 0x1000))) continue;
            for (uint32_t o = 0; o < 0x1000 - 16 && nh < 80; o += 4) { uint32_t w; memcpy(&w, q8 + o, 4); if (w != 0x528000c1u) continue;
              for (int k = 1; k <= 3; k++) { uint32_t bw; memcpy(&bw, q8 + o + 4 * (uint32_t)k, 4); if ((bw & 0xfc000000u) != 0x94000000u) continue; int64_t off = (int64_t)((int32_t)(bw << 6) >> 6) * 4; uint64_t tg = pg + o + 4 * (uint64_t)k + (uint64_t)off;
                for (int z = 0; z < ns; z++) if (tg <= setter[z] && setter[z] - tg < 0x40) { fprintf(stderr, "[e6c] %llx: mov w1,#6 ; bl %llx (setter %llx)\n", (unsigned long long)((pg + o) & 0xfffffffffull), (unsigned long long)(tg & 0xfffffffffull), (unsigned long long)(setter[z] & 0xfffffffffull)); nh++; }
                break; } } } }
        { /* 0.4.341 diagnostic: the thread blocked on an rw lock under OSMetaClass "Classes" (frames 7ba7904 <- 803aff0 <- 800d280): raw stack words around the 803aff0 frame; every pointer into kernel data is dereferenced (4 qwords) to find the lock and its state */
          for (uint64_t sb2 = 0xffffffe01a000000ull; sb2 < 0xffffffe01c000000ull; sb2 += 0x4000) { uint64_t pa7; uint8_t *q = NULL; uint8_t stk[0x4000]; int ok = 1;
            for (uint64_t o = 0; o < 0x4000 && ok; o += 0x1000) { if (a64_virt_to_phys(c7, sb2 + o, &pa7) || !(q = a64m_ram_at(m, pa7, 0x1000))) ok = 0; else memcpy(stk + o, q, 0x1000); }
            if (!ok) continue;
            for (uint64_t A = 0x100; A + 16 <= 0x4000; A += 16) { uint64_t lr; memcpy(&lr, stk + A + 8, 8); if ((lr & 0xfffffffffull) != 0x800d280ull) continue; { uint64_t pl2; memcpy(&pl2, stk + A - 8, 8); if ((pl2 & 0xfffffffffull) != 0x803aff0ull) continue; }
              fprintf(stderr, "[lockw] stack %llx frame +%llx (return into 800d280):", (unsigned long long)(sb2 & 0xfffffffffull), (unsigned long long)A);
              for (uint64_t W = A - 0x90; W < A + 0x20; W += 8) { uint64_t v; memcpy(&v, stk + W, 8); fprintf(stderr, " [%+d]%llx", (int)W - (int)A, (unsigned long long)v); }
              fprintf(stderr, "\n");
              for (uint64_t W = A - 0x90; W < A + 0x20; W += 8) { uint64_t v, pp2; memcpy(&v, stk + W, 8); uint8_t *dq;
                if ((v >> 36) != 0xffffffeull && (v >> 36) != 0xfffffffull) continue; if (v >= 0xfffffff007000000ull && v < 0xfffffff008ffffffull) continue; if ((v >> 12) == ((uint64_t)sb2 >> 12) || (v & 0xfffffffffffc0000ull) == (sb2 & 0xfffffffffffc0000ull)) continue;
                if (a64_virt_to_phys(c7, v, &pp2) || !(dq = a64m_ram_at(m, pp2, 32))) continue; uint64_t w4[4]; memcpy(w4, dq, 32);
                fprintf(stderr, "[lockw]   ptr %llx [%+d]: %llx %llx %llx %llx\n", (unsigned long long)v, (int)W - (int)A, (unsigned long long)w4[0], (unsigned long long)w4[1], (unsigned long long)w4[2], (unsigned long long)w4[3]); } } } }
        { /* 0.4.329: for frames whose lr is 0x814a91c (the platform-function wait helper): saved x19 = property key (OSSymbol/OSString, C string at +0x10), x21 = nub */
          static uint64_t seen[64]; int ns = 0;
          for (uint64_t sb2 = 0xffffffe01a000000ull; sb2 < 0xffffffe01c000000ull; sb2 += 0x4000) { uint64_t pa7; uint8_t *q = NULL; uint8_t stk[0x4000]; int ok = 1;
            for (uint64_t o = 0; o < 0x4000 && ok; o += 0x1000) { if (a64_virt_to_phys(c7, sb2 + o, &pa7) || !(q = a64m_ram_at(m, pa7, 0x1000))) ok = 0; else memcpy(stk + o, q, 0x1000); }
            if (!ok) continue;
            for (uint64_t A = 0x40; A + 16 <= 0x4000; A += 16) { uint64_t lr; memcpy(&lr, stk + A + 8, 8); if ((lr & 0xfffffffffull) != 0x814a91cull) continue;
              uint64_t x19, x21; memcpy(&x19, stk + A - 8, 8); memcpy(&x21, stk + A - 0x18, 8);
              char ks[48] = "?"; uint64_t sp1, pp, kp = 0; uint8_t *kq; if (!a64_virt_to_phys(c7, x19 + 0x10, &sp1) && (kq = a64m_ram_at(m, sp1, 8))) memcpy(&kp, kq, 8);
              if (kp && !a64_virt_to_phys(c7, kp, &pp) && (kq = a64m_ram_at(m, pp, 40))) { memcpy(ks, kq, 40); ks[40] = 0; for (int z = 0; z < 40 && ks[z]; z++) if ((unsigned char)ks[z] < 32 || (unsigned char)ks[z] > 126) ks[z] = '.'; }
              fprintf(stderr, "[fnwait] stack %llx key obj %llx \"%s\" nub %llx\n", (unsigned long long)(sb2 & 0xfffffffffull), (unsigned long long)x19, ks, (unsigned long long)x21); ns++; } }
          (void)seen; (void)ns; }
        for (int u = 0; u < nul; u++) { uint64_t t = 0xfffffff000000000ull | ul[u]; int cnt = 0; char out[600]; size_t on = 0; out[0] = 0;
          for (uint64_t a = t - 0x400; a < t && cnt < 4; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c7, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c7, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[70]; if (a64_virt_to_phys(c7, tg, &sp) || !(sq = a64m_ram_at(m, sp, 64))) continue; memcpy(buf, sq, 64); buf[64] = 0;
            int good = 1, n = 0; while (n < 64 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) good = 0; n++; } if (!good || n < 4) continue;
            on += (size_t)snprintf(out + on, sizeof out - on, " \"%s\"", buf); cnt++; }
          if (cnt) fprintf(stderr, "[stks] %llx:%s\n", (unsigned long long)ul[u], out); }
        }
      for (int wi = 0; wi < 4; wi++) if (c->wpc[wi] && c->wn[wi]) { /* 0.4.352: x1 of the watched indirect call as a C string */ char sx[64] = "?"; uint64_t pq; uint8_t *qq; if (!a64_virt_to_phys(c, c->wx1[wi], &pq) && (qq = a64m_ram_at(m, pq, 48))) { memcpy(sx, qq, 48); sx[48] = 0; for (int z = 0; z < 48; z++) if ((unsigned char)sx[z] && ((unsigned char)sx[z] < 32 || (unsigned char)sx[z] > 126)) sx[z] = '.'; }
        fprintf(stderr, "[wsym] pc %llx calls=%u last target %llx x0=%llx x1=%llx \"%s\"\n", (unsigned long long)c->wpc[wi], c->wn[wi], (unsigned long long)c->wtg[wi], (unsigned long long)c->wx0[wi], (unsigned long long)c->wx1[wi], sx); }
      if (c->wpc[0] == 0xfffffff008149510ull && c->wn[0] && c->wx0[0]) { /* 0.4.310 diagnostic: the DART mapper object: words +0xc0..+0x130 and the physical-range table (count [+0x118], pointer [+0x120], pairs start/end) the check at 0x850b600 walks */
        uint64_t ob = c->wx0[0]; fprintf(stderr, "[dobj] object %llx words +0xc0..+0x130:", (unsigned long long)ob);
        for (unsigned o = 0xc0; o <= 0x130; o += 8) { uint64_t pp; uint8_t *qq; uint64_t w = 0; if (!a64_virt_to_phys(c, ob + o, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&w, qq, 8); fprintf(stderr, " +%x=%llx", o, (unsigned long long)w); }
        uint64_t pp, cnt = 0, tb = 0; uint8_t *qq; if (!a64_virt_to_phys(c, ob + 0x118, &pp) && (qq = a64m_ram_at(m, pp, 8))) cnt = *(uint32_t *)qq; if (!a64_virt_to_phys(c, ob + 0x120, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&tb, qq, 8);
        fprintf(stderr, "\n[dobj] range table count %llu at %llx:", (unsigned long long)cnt, (unsigned long long)tb);
        for (uint64_t i = 0; i < cnt && i < 16; i++) { uint64_t a[2] = {0, 0}; for (int j = 0; j < 2; j++) if (!a64_virt_to_phys(c, tb + i * 16 + j * 8, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&a[j], qq, 8); fprintf(stderr, " [%llx..%llx]", (unsigned long long)a[0], (unsigned long long)a[1]); }
        fprintf(stderr, "\n"); }
      if (c->e6_n) { fprintf(stderr, "[e6h] b_error=6 first stored at pc %llx, lr=%llx; last 64 pcs:", (unsigned long long)c->e6_x0, (unsigned long long)(c->e6_lr & 0xfffffffffull)); for (unsigned hi = 1; hi < 64; hi++) if (c->e6_hist[hi] != c->e6_hist[hi - 1] + 4) fprintf(stderr, " %llx<%llx", (unsigned long long)(c->e6_hist[hi] & 0xfffffffffull), (unsigned long long)(c->e6_hist[hi - 1] & 0xfffffffffull)); fprintf(stderr, "\n"); }
      if (c->e6_n) { /* 0.4.375: the buf (x19) and the md device table entry (x22 - 12, 0x30 bytes) at the first b_error=6 store */
        uint64_t va[2] = { c->e6_reg[19], c->e6_reg[22] - 12 }; for (int vi = 0; vi < 2; vi++) { uint64_t pq; uint8_t *qq; size_t nb = vi ? 0x30 : 0xb0; if (!a64_virt_to_phys(c, va[vi], &pq) && (qq = a64m_ram_at(m, pq, nb))) { fprintf(stderr, "[e6%s] %llx:", vi ? "d" : "b", (unsigned long long)va[vi]); for (size_t z = 0; z < nb; z += 4) { uint32_t w; memcpy(&w, qq + z, 4); fprintf(stderr, " %x", w); } fprintf(stderr, "\n"); } else fprintf(stderr, "[e6%s] %llx unmapped\n", vi ? "d" : "b", (unsigned long long)va[vi]); }
        fprintf(stderr, "[e6r] x19=%llx x21=%llx x22=%llx\n", (unsigned long long)c->e6_reg[19], (unsigned long long)c->e6_reg[21], (unsigned long long)c->e6_reg[22]); }
      if (c->cap_n) { fprintf(stderr, "[caps] last 512 RETs before the assertion (ret pc:x0), oldest first, only those with x0 < 0x100 (index@pc:x0; 511 = the last):"); for (unsigned hi = 0; hi < 512; hi++) if (c->ret_snap_x0[hi] < 0x100) fprintf(stderr, " %u@%llx:%llx", hi, (unsigned long long)(c->ret_snap_pc[hi] & 0xfffffffffull), (unsigned long long)c->ret_snap_x0[hi]); fprintf(stderr, "\n"); }
      if (c->cap_n) { fprintf(stderr, "[caph] control-flow breaks (pc after a non-sequential jump) in the last %d steps before the assertion:", A64_PC_HIST); int cn = 0; for (unsigned hi = 1; hi < A64_PC_HIST; hi++) if (c->cap_hist[hi] != c->cap_hist[hi - 1] + 4 && c->cap_hist[hi]) { fprintf(stderr, " %llx<%llx", (unsigned long long)(c->cap_hist[hi] & 0xfffffffffull), (unsigned long long)(c->cap_hist[hi - 1] & 0xfffffffffull)); cn++; } fprintf(stderr, " (%d)\n", cn); }
      if (c->cap_n) { fprintf(stderr, "[cap] pc 7a43ddc x0..x3 = %llx %llx %llx %llx\n", (unsigned long long)c->cap_x[0], (unsigned long long)c->cap_x[1], (unsigned long long)c->cap_x[2], (unsigned long long)c->cap_x[3]);
        for (int ci = 0; ci < 4; ci++) { uint64_t pq; uint8_t *qq; char sx[100]; if (c->cap_x[ci] > 0x1000 && !a64_virt_to_phys(c, c->cap_x[ci], &pq) && (qq = a64m_ram_at(m, pq, 100))) { int n = 0; while (n < 99 && qq[n] >= 32 && qq[n] < 127) { sx[n] = (char)qq[n]; n++; } sx[n] = 0; if (n >= 3) fprintf(stderr, "[cap] x%d = \"%s\"\n", ci, sx); } } }
      { for (int wi = 0; wi < 4; wi++) if (c->wpc[wi]) fprintf(stderr, "[wcall] pc %llx last target %llx executions %u\n", (unsigned long long)(c->wpc[wi] & 0xfffffffffull), (unsigned long long)c->wtg[wi], c->wn[wi]); }
      { /* 0.4.305 diagnostic: (a) which parts of IODARTMapper::iovmMapMemory (0x850b0c8) ran: executed ranges 0x850b0c0..0x850b440; (b) where in memory is its vtable slot (qword whose low 36 bits = 0x850b0c8) and 400 qwords around it (low 36 bits), to read DART virtual methods +0x5c0/+0x5d0 */
        { const uint64_t lo = 0xfffffff007ed0800ull, hi = 0xfffffff007ed2000ull; int run = -1; uint64_t rs = lo; fprintf(stderr, "[dmx] executed ranges 7ed0800-7ed2000 (around load_init_program):");
          for (uint64_t a = lo; a <= hi; a += 4) { uint64_t co = a - c->cov_base; int ex = a < hi && c->cov && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7)));
            if (ex != run) { if (run == 1) fprintf(stderr, " %llx-%llx", (unsigned long long)(rs & 0xfffffffffull), (unsigned long long)((a - 4) & 0xfffffffffull)); run = ex; rs = a; } } fprintf(stderr, "\n"); }
        int nsl = 0;
        for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff00b000000ull && nsl < 3; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
          for (unsigned o = 0; o < 0x1000 && nsl < 3; o += 8) { uint64_t v; memcpy(&v, q + o, 8); if ((v & 0xfffffffffull) != 0x850b0c8ull) continue; nsl++; uint64_t sl = pg + o;
            fprintf(stderr, "[dmx] slot at %llx; qwords from -100..+300 (low 36 bits):", (unsigned long long)(sl & 0xfffffffffull));
            for (int k = -100; k < 300; k++) { uint64_t va = sl + (int64_t)k * 8, pp; uint8_t *qq; uint64_t w = 0; if (!a64_virt_to_phys(c, va, &pp) && (qq = a64m_ram_at(m, pp, 8))) memcpy(&w, qq, 8); fprintf(stderr, " %d:%llx", k, (unsigned long long)(w & 0xfffffffffull)); } fprintf(stderr, "\n"); } }
        if (!nsl) fprintf(stderr, "[dmx] no vtable slot with 0x850b0c8 found\n"); }
      { /* 0.4.303 diagnostic: find the format string "Map request failed" in kernel memory, find adrp+add pairs that reference its start (xrefs) and dump the code before the first xref as "[stop] code" lines (for dis.py) */
        static const char nd2[] = "Map request failed"; uint64_t sva = 0;
        for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff00b000000ull && !sva; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
          for (unsigned o = 0; o + sizeof nd2 < 0x1000; o++) if (q[o] == 'M' && !memcmp(q + o, nd2, sizeof nd2 - 1)) { unsigned st = o; while (st > 0 && q[st - 1]) st--; sva = pg + st; break; } }
        if (!sva) fprintf(stderr, "[xref] string 'Map request failed' not found\n");
        else { fprintf(stderr, "[xref] string start %llx\n", (unsigned long long)sva); int nx = 0; uint64_t first = 0;
          for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff009400000ull && nx < 6; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
            for (unsigned o = 0; o + 4 <= 0x1000 - 4 && nx < 6; o += 4) { uint32_t w, w2; memcpy(&w, q + o, 4); memcpy(&w2, q + o + 4, 4);
              if ((w & 0x9f000000u) != 0x90000000u) continue; int64_t im = (int64_t)(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3)); if (im & 0x100000) im -= 0x200000;
              uint64_t tg = ((pg + o) & ~0xfffull) + ((uint64_t)im << 12); if (tg != (sva & ~0xfffull)) continue;
              if ((w2 & 0xffc00000u) != 0x91000000u || ((w2 >> 5) & 31) != (w & 31) || ((w2 >> 10) & 0xfff) != (sva & 0xfff)) continue;
              fprintf(stderr, "[xref] %llx\n", (unsigned long long)(pg + o)); if (!first) first = pg + o; nx++; } }
          if (first) for (uint64_t a = (first & ~0xfull) - 0x500; a < first + 0x80; a += 16) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, a, &pa) || !(q = a64m_ram_at(m, pa, 16))) continue; uint32_t w[4]; memcpy(w, q, 16);
            fprintf(stderr, "[stop] code 0x%llx (kernel virtual 0x%llx): %x %x %x %x\n", (unsigned long long)a, (unsigned long long)a, w[0], w[1], w[2], w[3]); } } }
      { /* 0.4.204 diagnostic: the kext personality (Info.plist, if the prelinked XML is still in memory) of AppleSEPManager: IOClass key with that value, plus 700 bytes around it (matching keys: IONameMatch, IOProviderClass, IOResourceMatch ...) */
        static const char nd[] = "SmartIO"; int found = 0;
        for (uint64_t pg = 0xfffffff007000000ull; pg < 0xfffffff00b000000ull && found < 14; pg += 0x1000) { uint64_t pa; uint8_t *q; if (a64_virt_to_phys(c, pg, &pa) || !(q = a64m_ram_at(m, pa, 0x1000))) continue;
          for (unsigned o = 0; o + sizeof nd < 0x1000 && found < 14; o++) if (q[o] == nd[0] && !memcmp(q + o, nd, sizeof nd - 1)) { found++;
            char buf[501]; uint64_t va = pg + o - 250; for (int i = 0; i < 500; i++) { uint64_t pp; uint8_t *qq; char ch = '?'; if (!a64_virt_to_phys(c, va + i, &pp) && (qq = a64m_ram_at(m, pp, 1))) ch = (char)*qq; buf[i] = ch == 0 ? '|' : (ch < 32 || ch > 126) ? '.' : ch; } buf[500] = 0;
            fprintf(stderr, "[plist] at %llx: %s\n", (unsigned long long)((pg + o) & 0xfffffffffull), buf); } }
        if (!found) fprintf(stderr, "[plist] no '%s' found in 0xfffffff007000000..0xfffffff00b000000 (prelink info not in memory)\n", nd);
      }
      { /* 0.4.211 diagnostic: at the stop (the guest is in its own panic loop) walk the frame-pointer chain from x29 and print saved lrs with the bl target before each, plus strings referenced in the 0x200 bytes before each return address */
        uint64_t fp = c->x[29];
        for (int d = 0; d < 16 && fp; d++) { uint64_t pa, nf = 0, lr = 0; uint8_t *q; if (a64_virt_to_phys(c, fp, &pa) || !(q = a64m_ram_at(m, pa, 16))) { fprintf(stderr, "[pfr] #%d fp %llx unmapped\n", d, (unsigned long long)fp); break; } memcpy(&nf, q, 8); memcpy(&lr, q + 8, 8);
          uint64_t t = 0xfffffff000000000ull | (lr & 0xfffffffffull); fprintf(stderr, "[pfr] #%d fp %llx lr %llx", d, (unsigned long long)fp, (unsigned long long)(t & 0xfffffffffull));
          uint64_t pp; uint8_t *iq; uint32_t ins = 0; if (!a64_virt_to_phys(c, t - 4, &pp) && (iq = a64m_ram_at(m, pp, 4))) { memcpy(&ins, iq, 4); if ((ins >> 26) == 0x25) fprintf(stderr, " (bl %llx)", (unsigned long long)((t - 4 + (uint64_t)(((int64_t)(int32_t)(ins << 6) >> 6) * 4)) & 0xfffffffffull)); }
          int cnt = 0; for (uint64_t a = t - 0x200; a < t && cnt < 4; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
            if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
            if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
            int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
            uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
            int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
            fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); cnt++; }
          fprintf(stderr, "\n"); if (nf <= fp) break; fp = nf; }
      }
      { /* 0.4.393 diagnostic: find the kernel's vm_map entries of the user processes by their (start,end) qword pair and print the map (entry = {prev,next,start,end,...}) */
        static const uint64_t pats[2][2] = { { 0x105aa8000ull, 0x105ad8000ull }, { 0x102698000ull, 0x10269c000ull } };
        for (int pi = 0; pi < 2; pi++) { int hits = 0; for (uint64_t pa = 0x800000000ull; pa < 0x800000000ull + (3ull << 30) && hits < 3; pa += 0x100000) { uint8_t *q = a64m_ram_at(m, pa, 0x100000); if (!q) break;
            for (uint32_t o = 16; o + 16 <= 0x100000 && hits < 3; o += 8) { uint64_t w0, w1; memcpy(&w0, q + o, 8); if (w0 != pats[pi][0]) continue; memcpy(&w1, q + o + 8, 8); if (w1 != pats[pi][1]) continue; uint64_t prv, nxt; memcpy(&prv, q + o - 16, 8); memcpy(&nxt, q + o - 8, 8); if ((prv >> 36) != 0xffffffeull && (prv >> 36) != 0xfffffffull) continue; if ((nxt >> 36) != 0xffffffeull && (nxt >> 36) != 0xfffffffull) continue;
              hits++; fprintf(stderr, "[vmmap] pattern %d found at phys %llx (entry links prev=%llx next=%llx); walking back to the head and forward:\n[vmmap]", pi, (unsigned long long)(pa + o - 16), (unsigned long long)prv, (unsigned long long)nxt);
              uint64_t cur = prv; int steps = 0; uint64_t first = 0; while (steps++ < 400) { uint64_t pq; uint8_t *qq; if (a64_virt_to_phys(c, cur, &pq) || !(qq = a64m_ram_at(m, pq, 32))) break; uint64_t l[4]; memcpy(l, qq, 32); first = cur; if (l[0] == cur || l[0] < 0xffffffe000000000ull) break; cur = l[0]; if (steps > 380) break; }
              uint64_t e = first; for (int st = 0; st < 400; st++) { uint64_t pq; uint8_t *qq; if (a64_virt_to_phys(c, e, &pq) || !(qq = a64m_ram_at(m, pq, 32))) break; uint64_t l[4]; memcpy(l, qq, 32); fprintf(stderr, " [%llx-%llx]", (unsigned long long)l[2], (unsigned long long)l[3]); e = l[1]; if (e == first) break; }
              fprintf(stderr, "\n"); } } } }
      { /* 0.4.381: syscalls (SVC) and aborts from user mode (EL0) */
        fprintf(stderr, "[el0] SVC count %u; first syscall numbers (x16@pc):", c->svc_n); for (unsigned q = 0; q < c->svc_n && q < 96; q++) fprintf(stderr, " %d@%llx", (int)c->svc_nr[q], (unsigned long long)(c->svc_pc[q] & 0xfffffffffull)); fprintf(stderr, "\n");
        if (c->el0r.ok || c->el0l_n) { fprintf(stderr, "[el0r] regs at the last EL0 abort:"); for (int q = 0; q < 31; q++) fprintf(stderr, " x%d=%llx", q, (unsigned long long)c->el0r.x[q]); fprintf(stderr, "\n[el0r] code at %llx (%s):", (unsigned long long)c->el0r.codeva, c->el0r.ok ? "readable" : "NOT readable"); for (int q = 0; q < 8; q++) fprintf(stderr, " %016llx", (unsigned long long)c->el0r.code[q]); fprintf(stderr, "\n"); }
        if (c->el0r.ok) fprintf(stderr, "[el0r] at the last abort: tpidr_el0=%llx tpidrro_el0=%llx tpidr_el1(thread)=%llx sp=%llx\n", (unsigned long long)c->el0r.tp0, (unsigned long long)c->el0r.tpro, (unsigned long long)c->el0r.tp1, (unsigned long long)c->el0r.sp);
        if (c->el0r.ok) { fprintf(stderr, "[el0r] user frame chain (fp:lr):"); for (unsigned q = 0; q < c->el0r.frn; q++) fprintf(stderr, " %llx:%llx", (unsigned long long)c->el0r.fr[q][0], (unsigned long long)c->el0r.fr[q][1]); fprintf(stderr, "\n[el0r] user stack at the abort:"); for (int q = 0; q < 24; q++) fprintf(stderr, " %llx", (unsigned long long)c->el0r.stk[q]); fprintf(stderr, "\n"); }
        fprintf(stderr, "[el0] last abort ran in address space <%llx>\n", (unsigned long long)((c->el0_last_ttbr >> 48) << 24 | ((c->el0_last_ttbr >> 14) & 0xffffffull)));
        fprintf(stderr, "[paths] files the user process stat()/open()ed (first 39 bytes of each path), in order:"); for (unsigned q = c->svc_n > 4096 ? c->svc_n - 4096 : 0; q < c->svc_n; q++) if (c->svl_str[q & 4095][0]) { char pb[41]; memcpy(pb, c->svl_str[q & 4095], 40); pb[40] = 0; for (int z = 0; z < 40; z++) if (pb[z] && (pb[z] < 32 || pb[z] > 126)) pb[z] = '?'; fprintf(stderr, " <%llx>%d:%s=%llx", (unsigned long long)((c->svl_ttbr[q & 4095] >> 48) << 24 | ((c->svl_ttbr[q & 4095] >> 14) & 0xffffffull)), (int)c->svl_nr[q & 4095], pb, (unsigned long long)c->svl_ret[q & 4095]); } fprintf(stderr, "\n");
        fprintf(stderr, "[el0] LAST syscalls (nr(x0,x1,x2)=ret[*x1]@pc), oldest first:"); for (unsigned q = c->svc_n > 64 ? c->svc_n - 64 : 0; q < c->svc_n; q++) fprintf(stderr, " <%llx>%d(%llx,%llx,%llx)=%llx[*x1=%llx]@%llx", (unsigned long long)((c->svl_ttbr[q & 4095] >> 48) << 24 | ((c->svl_ttbr[q & 4095] >> 14) & 0xffffffull)), (int)c->svl_nr[q & 4095], (unsigned long long)c->svl_x0[q & 4095], (unsigned long long)c->svl_x1[q & 4095], (unsigned long long)c->svl_x2[q & 4095], (unsigned long long)c->svl_ret[q & 4095], (unsigned long long)c->svl_aux[q & 4095], (unsigned long long)(c->svl_pc[q & 4095] & 0xfffffffffull)); fprintf(stderr, "\n");
        fprintf(stderr, "[vmops] user VM operations of the last 4096 syscalls (nr(x0,x1,x2,x3)=ret[*x1]): mmap 197 / munmap 73 / mprotect 74 / mach vm_allocate -10 / deallocate -12 / protect -14 / map -15:"); for (unsigned q = c->svc_n > 4096 ? c->svc_n - 4096 : 0; q < c->svc_n; q++) { int nr = (int)c->svl_nr[q & 4095]; if (nr == 59 || nr == 244 || nr == 380 || nr == 1 || nr == 197 || nr == 73 || nr == 74 || nr == -10 || nr == -12 || nr == -14 || nr == -15) fprintf(stderr, " <%llx>%d(%llx,%llx,%llx,%llx)=%llx[%llx]", (unsigned long long)((c->svl_ttbr[q & 4095] >> 48) << 24 | ((c->svl_ttbr[q & 4095] >> 14) & 0xffffffull)), nr, (unsigned long long)c->svl_x0[q & 4095], (unsigned long long)c->svl_x1[q & 4095], (unsigned long long)c->svl_x2[q & 4095], (unsigned long long)c->svl_x3[q & 4095], (unsigned long long)c->svl_ret[q & 4095], (unsigned long long)c->svl_aux[q & 4095]); } fprintf(stderr, "\n");
        for (unsigned q = c->el0l_n > 8 ? c->el0l_n - 8 : 0; q < c->el0l_n; q++) fprintf(stderr, "[el0] LAST abort %u <%llx> step %llu esr=%x elr=%llx far=%llx x0=%llx x1=%llx sp=%llx\n", q, (unsigned long long)((c->el0l[q & 7].ttbr >> 48) << 24 | ((c->el0l[q & 7].ttbr >> 14) & 0xffffffull)), (unsigned long long)c->el0l[q & 7].steps, c->el0l[q & 7].esr, (unsigned long long)c->el0l[q & 7].elr, (unsigned long long)c->el0l[q & 7].far, (unsigned long long)c->el0l[q & 7].x0, (unsigned long long)c->el0l[q & 7].x1, (unsigned long long)c->el0l[q & 7].sp0);
        if (c->el0l_n) { char wb[900]; uint64_t sv = c->ttbr0_el1; unsigned li = (c->el0l_n - 1) & 7; c->ttbr0_el1 = c->el0l[li].ttbr; a64_walk_trace(c, c->el0l[li].far, wb, sizeof wb); c->ttbr0_el1 = sv; fprintf(stderr, "[el0w] 0.4.395 page-table walk of the last EL0 fault address under its own TTBR0 (0x%llx): %s\n", (unsigned long long)c->el0l[li].ttbr, wb); }
        if (c->el0l_n) { unsigned li = (c->el0l[(c->el0l_n - 1) & 7].ttbr, (c->el0l_n - 1) & 7); uint64_t tt = c->el0l[li].ttbr, fv = c->el0l[li].far, tb = (tt & 0x0000FFFFFFFFFFFEull) & ~0x3full, d = 0; uint8_t *q; int okw = 1;
            for (int lv = 1; lv <= 2 && okw; lv++) { uint64_t idx = lv == 1 ? ((fv >> 36) & 7) : ((fv >> 25) & 2047); if (!(q = a64m_ram_at(m, tb + idx * 8, 8))) { okw = 0; break; } memcpy(&d, q, 8); tb = d & 0x0000FFFFFFFFC000ull; }
            if (okw) { unsigned i3 = (unsigned)((fv >> 14) & 2047); fprintf(stderr, "[el0w3] L3 table at phys 0x%llx, entries around index %u:", (unsigned long long)tb, i3); for (int k = -6; k <= 6; k++) { uint64_t e = 0; if ((q = a64m_ram_at(m, tb + (uint64_t)((int)i3 + k) * 8, 8))) memcpy(&e, q, 8); fprintf(stderr, " [%d]=%llx", (int)i3 + k, (unsigned long long)e); } fprintf(stderr, "\n"); } }
        if (c->ex_n) { fprintf(stderr, "[exc-calls] 0.4.397 first-called kernel bl targets since the last EL0 data abort (far=0x%llx), in order (%u):", (unsigned long long)c->ex_far, c->ex_n); for (unsigned q = 0; q < c->ex_n; q++) fprintf(stderr, " %llx", (unsigned long long)c->ex_list[q] & 0xfffffffffull); fprintf(stderr, "\n"); }
        if (c->ex_ri) { fprintf(stderr, "[exc-ring] 0.4.403 last bl (site>target) of the thread that took the last EL0 data abort (thread 0x%llx), newest last:", (unsigned long long)c->ex_thr); for (unsigned q = c->ex_ri > 256 ? c->ex_ri - 256 : 0; q < c->ex_ri; q++) fprintf(stderr, " %llx>%llx", (unsigned long long)c->ex_rs[q & 255] & 0xfffffffffull, (unsigned long long)c->ex_rt[q & 255] & 0xfffffffffull); fprintf(stderr, "\n"); }
        if (c->ex_sn) { fprintf(stderr, "[exc-ret] 0.4.405 last 256 bl targets of that thread before its LAST return to EL0 (to 0x%llx, x0=%llx, %llu returns of that thread so far), newest last:", (unsigned long long)c->ex_sret, (unsigned long long)c->ex_sx0, (unsigned long long)c->ex_sn); for (unsigned q = c->ex_sri > 256 ? c->ex_sri - 256 : 0; q < c->ex_sri; q++) fprintf(stderr, " %llx", (unsigned long long)c->ex_sr[q & 255] & 0xfffffffffull); fprintf(stderr, "\n"); }
        for (unsigned q = c->wr_n > 32 ? c->wr_n - 32 : 0; q < c->wr_n; q++) { char tb[300]; size_t tn = 0; for (const char *z = c->wr_txt[q & 31]; *z && tn < 280; z++) { unsigned char ch = (unsigned char)*z; if (ch >= 32 && ch < 127) tb[tn++] = (char)ch; else tn += (size_t)snprintf(tb + tn, 8, "\\x%02x", ch); } tb[tn] = 0; fprintf(stderr, "[con-write] 0.4.406 write(fd %u) by TTBR0 0x%llx: \"%s\"\n", c->wr_fd[q & 31], (unsigned long long)c->wr_ttbr[q & 31], tb); }
        for (unsigned q = 0; q < c->exc_el0_n; q++) fprintf(stderr, "[el0] abort #%u step %llu esr=%x elr=%llx far=%llx x0=%llx x1=%llx sp_el0=%llx\n", q, (unsigned long long)c->exc_el0[q].steps, c->exc_el0[q].esr, (unsigned long long)c->exc_el0[q].elr, (unsigned long long)c->exc_el0[q].far, (unsigned long long)c->exc_el0[q].x0, (unsigned long long)c->exc_el0[q].x1, (unsigned long long)c->exc_el0[q].sp0); }
      { /* 0.4.380 diagnostic: look for the kernel's panic log text in guest RAM ("panic(cpu N caller ...): <message>") - scan physical RAM in 1 MiB chunks */
        int hits = 0; for (uint64_t pa = 0x800000000ull; pa < 0x800000000ull + (3ull << 30) && hits < 3; pa += 0x100000) { uint8_t *q = a64m_ram_at(m, pa, 0x100000); if (!q) break;
          for (uint32_t o = 0; o + 12 < 0x100000 && hits < 3; o++) if (q[o] == 'p' && !memcmp(q + o, "panic(cpu ", 10)) { char sx[600]; int n = 0; while (n < 599 && o + n < 0x100000 && q[o + n] >= 9 && q[o + n] < 127) { sx[n] = (char)q[o + n]; n++; } sx[n] = 0; fprintf(stderr, "[ptext] phys %llx: %s\n", (unsigned long long)(pa + o), sx); hits++; } }
        if (!hits) fprintf(stderr, "[ptext] no 'panic(cpu ' text found in the first 3 GiB of RAM\n"); }
      { /* 0.4.212 diagnostic: frame chain at the FIRST entry into panic() (0x7a44164), i.e. the code that triggered the first panic ("thread_invoke: preemption_level 1 ...") */
        if (!c->snap_n) fprintf(stderr, "[pfirst] panic() was never entered\n");
        else { fprintf(stderr, "[pfirst] panic() first entered at step %llu: lr=%llx sp=%llx x0=%llx\n", (unsigned long long)c->snap_step, (unsigned long long)(c->snap_lr & 0xfffffffffull), (unsigned long long)c->snap_sp, (unsigned long long)c->snap_x0);
          { /* 0.4.379: the first panic's x0 as text, and as a pointer to text */
            uint64_t cand[2] = { c->snap_x0, 0 }; uint64_t pa0, pv = 0; uint8_t *q0; if (!a64_virt_to_phys(c, c->snap_x0, &pa0) && (q0 = a64m_ram_at(m, pa0, 8))) { memcpy(&pv, q0, 8); cand[1] = pv; }
            for (int ci = 0; ci < 2; ci++) { uint64_t pq; uint8_t *qq; char sx[260]; if (cand[ci] > 0x1000 && !a64_virt_to_phys(c, cand[ci], &pq) && (qq = a64m_ram_at(m, pq, 256))) { int n = 0; while (n < 255 && qq[n] >= 9 && qq[n] < 127) { sx[n] = (char)qq[n]; n++; } sx[n] = 0; if (n >= 4) fprintf(stderr, "[pfirst] x0%s as text: \"%s\"\n", ci ? " -> pointer" : "", sx); } } }
          uint64_t fp = c->snap_fp;
          for (int d = 0; d < 20 && fp; d++) { uint64_t pa, nf = 0, lr = 0; uint8_t *q; if (a64_virt_to_phys(c, fp, &pa) || !(q = a64m_ram_at(m, pa, 16))) break; memcpy(&nf, q, 8); memcpy(&lr, q + 8, 8);
            uint64_t t = 0xfffffff000000000ull | (lr & 0xfffffffffull); fprintf(stderr, "[pfirst] #%d lr %llx", d, (unsigned long long)(t & 0xfffffffffull));
            uint64_t pp; uint8_t *iq; uint32_t ins = 0; if (!a64_virt_to_phys(c, t - 4, &pp) && (iq = a64m_ram_at(m, pp, 4))) { memcpy(&ins, iq, 4); if ((ins >> 26) == 0x25) fprintf(stderr, " (bl %llx)", (unsigned long long)((t - 4 + (uint64_t)(((int64_t)(int32_t)(ins << 6) >> 6) * 4)) & 0xfffffffffull)); }
            int cnt = 0; for (uint64_t a = t - 0x300; a < t && cnt < 5; a += 4) { uint32_t i1 = 0, i2 = 0; uint64_t p1, p2; uint8_t *r1, *r2;
              if (a64_virt_to_phys(c, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4)) || a64_virt_to_phys(c, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i1, r1, 4); memcpy(&i2, r2, 4);
              if ((i1 & 0x9f000000u) != 0x90000000u || (i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
              int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff);
              uint64_t sp; uint8_t *sq; char buf[64]; if (a64_virt_to_phys(c, tg, &sp) || !(sq = a64m_ram_at(m, sp, 60))) continue; memcpy(buf, sq, 60); buf[60] = 0;
              int ok = 1, n = 0; while (n < 60 && buf[n]) { if ((unsigned char)buf[n] < 32 || (unsigned char)buf[n] > 126) ok = 0; n++; } if (!ok || n < 4) continue;
              fprintf(stderr, " @%llx\"%s\"", (unsigned long long)(a & 0xfffffffffull), buf); cnt++; }
            fprintf(stderr, "\n"); if (nf <= fp) break; fp = nf; } }
      }
      fprintf(stderr, "[aic-info] %s\n", a64m_aic_info(m));
      fprintf(stderr, "[aicw] distinct AIC register writes (+offset:count|OR of values):%s\n", a64m_aic_writes(m));
      { /* 0.4.251 diagnostic: where does the kernel build "voltage-states%d" (the PMGR perf-domain frequency tables)? find the string, then every ADRP+ADD reference in the code range */
        A64 *c4 = a64m_cpu(m); uint64_t sva = 0, pa4; uint8_t *q8; static const char nd[] = "AppleARMCPU-AllCPU-Initialized";   /* 0.4.317: where is init_program_name used (bsd_utaskbootstrap/load_init_program)? are those executed (E)? */
        for (uint64_t va = 0xfffffff007000000ull; va < 0xfffffff00a800000ull && !sva; va += 0x4000) { if (a64_virt_to_phys(c4, va, &pa4) != 0 || !(q8 = a64m_ram_at(m, pa4, 0x4000))) continue;
          for (uint64_t o = 0; o + 20 < 0x4000; o++) if (!memcmp(q8 + o, nd, sizeof nd - 1) && q8[o + sizeof nd - 1] == 0) { sva = va + o; break; } }
        fprintf(stderr, "[vs] string at %llx; refs:", (unsigned long long)(sva & 0xfffffffffull));
        int nr = 0; for (uint64_t a = c4->cov_base; sva && a < c4->cov_base + c4->cov_len && nr < 40; a += 4) { uint32_t i1 = 0, i2 = 0; uint8_t *r1, *r2; uint64_t p1, p2; if (a64_virt_to_phys(c4, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4); if ((i1 & 0x9f000000u) != 0x90000000u) continue;
          if (a64_virt_to_phys(c4, a + 4, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) continue; memcpy(&i2, r2, 4); if ((i2 & 0xff800000u) != 0x91000000u || ((i2 >> 5) & 31) != (i1 & 31)) continue;
          int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; uint64_t tg = ((a & ~0xfffull) + (uint64_t)(im << 12)) + ((i2 >> 10) & 0xfff); if (tg == sva) { fprintf(stderr, " %llx(%s)", (unsigned long long)(a & 0xfffffffffull), (c4->cov && (a - c4->cov_base) < c4->cov_len && (c4->cov[(a - c4->cov_base) >> 5] & (1u << (((a - c4->cov_base) >> 2) & 7)))) ? "E" : "-"); nr++; } }
        fprintf(stderr, "\n"); }
      { /* 0.4.324 diagnostic: users of the global OSSymbol "AppleARMCPU-AllCPU-Initialized" stored at 0xfffffff00914e638 (static init at 0x814a630): adrp page 0x914e000 followed within 8 instructions by ldr/str [rd,#0x638]; (E)=executed */
        A64 *c8 = a64m_cpu(m); fprintf(stderr, "[gsym] users of 0x914e638:");
        int nu = 0; for (uint64_t a = c8->cov_base; a < c8->cov_base + c8->cov_len && nu < 40; a += 4) { uint32_t i1 = 0; uint8_t *r1; uint64_t p1; if (a64_virt_to_phys(c8, a, &p1) || !(r1 = a64m_ram_at(m, p1, 4))) continue; memcpy(&i1, r1, 4); if ((i1 & 0x9f000000u) != 0x90000000u) continue;
          int64_t im = ((int64_t)(((i1 >> 5) & 0x7ffff) << 2 | ((i1 >> 29) & 3)) << 43) >> 43; if ((((a & ~0xfffull) + (uint64_t)(im << 12)) & 0xfffffffffull) != 0x914e000ull) continue;
          for (int k = 1; k <= 8; k++) { uint32_t w = 0; uint8_t *r2; uint64_t p2; if (a64_virt_to_phys(c8, a + 4 * k, &p2) || !(r2 = a64m_ram_at(m, p2, 4))) break; memcpy(&w, r2, 4);
            if ((w & 0xff800000u) == 0xf9000000u && ((w >> 5) & 31) == (i1 & 31) && ((w >> 10) & 0xfff) == 0xc7) { uint64_t co = (a + 4 * k) - c8->cov_base; fprintf(stderr, " %llx%s%s", (unsigned long long)((a + 4 * k) & 0xfffffffffull), (w & 0x400000u) ? "L" : "S", (c8->cov && co < c8->cov_len && (c8->cov[co >> 5] & (1u << ((co >> 2) & 7)))) ? "(E)" : "(-)"); nu++; break; } } }
        fprintf(stderr, "\n"); }
      { /* 0.4.326: AppleARMCPU "all CPUs initialized" counter 0xfffffff0091bac38 */ A64 *c9 = a64m_cpu(m); uint64_t pa9; uint8_t *q9; uint32_t v9 = 0xdeadbeef; if (a64_virt_to_phys(c9, 0xfffffff0091bac38ull, &pa9) == 0 && (q9 = a64m_ram_at(m, pa9, 4))) memcpy(&v9, q9, 4); fprintf(stderr, "[cpucnt] counter at 0x91bac38 = %u\n", v9); }
      { /* 0.4.272 diagnostic: constant that AppleAVD::start stores to [this+0x100] when avd-version byte == 2 (adrp 0x8bfe6a4): 0xfffffff00716e400 */
        A64 *c6 = a64m_cpu(m); fprintf(stderr, "[avdc] 0xfffffff00716e3f0:"); for (uint64_t ad = 0xfffffff00716e3f0ull; ad < 0xfffffff00716e420ull; ad += 4) { uint64_t pa6; uint8_t *q6; uint32_t v6 = 0; if (a64_virt_to_phys(c6, ad, &pa6) == 0 && (q6 = a64m_ram_at(m, pa6, 4))) memcpy(&v6, q6, 4); fprintf(stderr, " %08x", v6); } fprintf(stderr, "\n"); }
      { /* 0.4.271 diagnostic: strings used by AppleAVD::start revision code (0x8bfe5c8..): 0xfffffff0076926b5.. */
        A64 *c5 = a64m_cpu(m); fprintf(stderr, "[avdstr]"); for (int rr_ = 0; rr_ < 2; rr_++) for (uint64_t ad = rr_ ? 0xfffffff0074d3b80ull : 0xfffffff0074d7300ull; ad < (rr_ ? 0xfffffff0074d3c80ull : 0xfffffff0074d7440ull); ad++) { uint64_t pa5; uint8_t *q5; int ch = 0; if (a64_virt_to_phys(c5, ad, &pa5) == 0 && (q5 = a64m_ram_at(m, pa5, 1))) ch = *q5; fputc(ch ? ch : '|', stderr); } fputc('\n', stderr); }
      { /* 0.4.261 diagnostic: AppleH10CamIn::start loads fISPVersion from [table+0x204] with table = 0xfffffff0079ab1e8 (adrp/add at 0x8c51824); print the table words */
        A64 *c4 = a64m_cpu(m); uint64_t pa4; uint8_t *q7; fprintf(stderr, "[isp] table 0xfffffff0079ab1e8 words:");
        for (int w = 0; w < 0x240; w += 4) { uint32_t v = 0; if (a64_virt_to_phys(c4, 0xfffffff0079ab1e8ull + (uint64_t)w, &pa4) == 0 && (q7 = a64m_ram_at(m, pa4, 4))) memcpy(&v, q7, 4); if (v) fprintf(stderr, " +%x=%x", w, v); } fprintf(stderr, "\n"); }
      { /* 0.4.246 diagnostic: global 0xfffffff00914f7c8 (= x26+0x308 of the CLPC ANE function) holds the object the forwarder 0x8187884 calls through [obj+0x88]: print it and the object's words +0x00..+0x100 */
        A64 *c3 = a64m_cpu(m); uint64_t pa3, gp = 0; uint8_t *q6; if (a64_virt_to_phys(c3, 0xfffffff00914f7c8ull, &pa3) == 0 && (q6 = a64m_ram_at(m, pa3, 8))) memcpy(&gp, q6, 8);
        fprintf(stderr, "[obj] [0xfffffff00914f7c8] = %llx\n", (unsigned long long)gp);
        if (gp) { uint64_t pv = 0; if (a64_virt_to_phys(c3, gp + 0x88, &pa3) == 0 && (q6 = a64m_ram_at(m, pa3, 8))) memcpy(&pv, q6, 8); fprintf(stderr, "[prov] provider %llx words:", (unsigned long long)pv); for (int w = 0; pv && w < 0x1c0; w += 8) { uint64_t v = 0; if (a64_virt_to_phys(c3, pv + (uint64_t)w, &pa3) == 0 && (q6 = a64m_ram_at(m, pa3, 8))) memcpy(&v, q6, 8); fprintf(stderr, " +%x=%llx", w, (unsigned long long)v); } fprintf(stderr, "\n"); { /* 0.4.248: ApplePMGR perf-domain table (code at 0x8181c94 reads [pmgr+0xe50] = array of 0xf0-byte domains, [pmgr+0xe5c+perfDomainID] = index byte, domain +8 = state count) */
          uint64_t tb = 0; uint8_t idx[24] = { 0 }; uint8_t *q7; if (a64_virt_to_phys(c3, pv + 0xe50, &pa3) == 0 && (q7 = a64m_ram_at(m, pa3, 8))) memcpy(&tb, q7, 8);
          for (int w = 0; w < 24; w++) { if (a64_virt_to_phys(c3, pv + 0xe5c + (uint64_t)w, &pa3) == 0 && (q7 = a64m_ram_at(m, pa3, 1))) idx[w] = *q7; }
          for (int d2 = 0; tb && d2 < 5; d2++) { uint64_t f88 = 0, f90 = 0, f70 = 0; uint64_t base = tb + (uint64_t)d2 * 0xf0; uint64_t ptrs[3]; const int offs[3] = { 0x88, 0x90, 0x70 };
            for (int z = 0; z < 3; z++) { uint64_t v = 0; if (a64_virt_to_phys(c3, base + (uint64_t)offs[z], &pa3) == 0 && (q7 = a64m_ram_at(m, pa3, 8))) memcpy(&v, q7, 8); ptrs[z] = v; }
            (void)f88; (void)f90; (void)f70;
            for (int z = 0; z < 3; z++) { fprintf(stderr, "[pdom] #%d [+%x]->%llx:", d2, offs[z], (unsigned long long)ptrs[z]); for (int w = 0; w < 0x50; w += 8) { uint64_t v = 0; if (ptrs[z] && a64_virt_to_phys(c3, (ptrs[z] >= 0xffffffe000000000ull ? ptrs[z] : (0xfffffff000000000ull | (ptrs[z] & 0xfffffffffull))) + (uint64_t)w, &pa3) == 0 && (q7 = a64m_ram_at(m, pa3, 8))) memcpy(&v, q7, 8); fprintf(stderr, " %llx", (unsigned long long)v); } fprintf(stderr, "\n"); } }
          fprintf(stderr, "[pdom] table %llx index bytes:", (unsigned long long)tb); for (int w = 0; w < 24; w++) fprintf(stderr, " %02x", idx[w]); fprintf(stderr, "\n");
          for (int d2 = 0; tb && d2 < 16; d2++) { fprintf(stderr, "[pdom] #%d:", d2); for (int w = 0; w < 0xa0; w += 8) { uint64_t v = 0; if (a64_virt_to_phys(c3, tb + (uint64_t)d2 * 0xf0 + (uint64_t)w, &pa3) == 0 && (q7 = a64m_ram_at(m, pa3, 8))) memcpy(&v, q7, 8); fprintf(stderr, " +%x=%llx", w, (unsigned long long)v); } fprintf(stderr, "\n"); } }
          fprintf(stderr, "[obj] words:"); for (int w = 0; w < 0x100; w += 8) { uint64_t v = 0; if (a64_virt_to_phys(c3, gp + (uint64_t)w, &pa3) == 0 && (q6 = a64m_ram_at(m, pa3, 8))) memcpy(&v, q6, 8); fprintf(stderr, " +%x=%llx", w, (unsigned long long)v); } fprintf(stderr, "\n"); } }
      { /* 0.4.232 diagnostic: the AppleSPMI object itself (first qword = vtable pointer 0xfffffff00795ceb8, PAC bits ignored): print its words +0x100..+0x1a0 (+0x110 flag, +0x168 interrupt-status bit, +0x180 timeout ms, +0x184 wait flag, +0x18c retries) */
        int nf = 0; for (uint64_t pb = 0x800000000ull; nf < 3; pb += 0x100000ull) { uint8_t *q5 = a64m_ram_at(m, pb, 0x100000); if (!q5) break;
          for (uint64_t o = 0; o + 0x1c0 <= 0x100000 && nf < 3; o += 8) { uint64_t v; memcpy(&v, q5 + o, 8); if ((v & 0xfffffffffull) == 0x0795ceb8ull && (v >> 36) != 0) { nf++; fprintf(stderr, "[spo] object at pa %llx:", (unsigned long long)(pb + o)); for (int w = 0x100; w < 0x1a0; w += 4) { uint32_t x; memcpy(&x, q5 + o + w, 4); fprintf(stderr, " +%x=%x", w, x); } fprintf(stderr, "\n"); } } }
        if (!nf) fprintf(stderr, "[spo] AppleSPMI object not found in RAM\n"); }
      { /* 0.4.231 diagnostic: AppleSPMI vtable (live, after the chained-fixup rebase): find the qword whose low 32 bits are 0x08982b18 (the retry/timeout handler = vtable slot 0x630), print slots 0x618..0x640 and the first 0x140 bytes of the code of slot 0x618 */
        A64 *c2 = a64m_cpu(m); uint64_t pa2, slot = 0; int found = 0;
        for (uint64_t va = 0xfffffff007000000ull; va < 0xfffffff00a800000ull && !found; va += 0x4000) { uint8_t *q2; if (a64_virt_to_phys(c2, va, &pa2) != 0 || !(q2 = a64m_ram_at(m, pa2, 0x4000))) continue;
          for (uint64_t o = 0; o < 0x4000; o += 8) { uint64_t v; memcpy(&v, q2 + o, 8); if ((v & 0xffffffffull) == 0x08982b18ull && (v >> 36) != 0) { slot = va + o; found = 1; break; } } }
        if (!found) fprintf(stderr, "[vt] no vtable slot pointing to 0x8982b18 found\n");
        else { fprintf(stderr, "[vt] slot 0x630 at %llx; slots:", (unsigned long long)slot); uint64_t t618 = 0;
          for (int d = -3; d <= 2; d++) { uint64_t v = 0; uint8_t *q3; if (a64_virt_to_phys(c2, slot + (uint64_t)(d * 8), &pa2) == 0 && (q3 = a64m_ram_at(m, pa2, 8))) memcpy(&v, q3, 8); uint64_t t = 0xfffffff000000000ull | (v & 0xfffffffffull); fprintf(stderr, " [0x%x]=%llx", 0x630 + d * 8, (unsigned long long)t); if (d == -3) t618 = t; }
          fprintf(stderr, "\n"); for (int blk = 0; blk < 3; blk++) { fprintf(stderr, "[vt] code %llx:", (unsigned long long)(t618 + (uint64_t)blk * 0x80)); for (int w = 0; w < 32; w++) { uint32_t x = 0; uint8_t *q4; if (a64_virt_to_phys(c2, t618 + (uint64_t)blk * 0x80 + 4u * (uint64_t)w, &pa2) == 0 && (q4 = a64m_ram_at(m, pa2, 4))) memcpy(&x, q4, 4); fprintf(stderr, " %08x", x); } fprintf(stderr, "\n"); } } }
      fprintf(stderr, "[i2c] last accesses:%s\n", a64m_i2c_trace());
      if (c->cov) { static const uint64_t cr[2][2] = { { 0xfffffff008cbf000ull, 0xfffffff008cc3000ull }, { 0xfffffff0082db000ull, 0xfffffff0082dba40ull } }; for (int ci = 0; ci < 2; ci++) { fprintf(stderr, "[cov-ans] 0.4.425 executed instruction runs (start-end of the last word) in 0x%llx..0x%llx (%s):", (unsigned long long)cr[ci][0], (unsigned long long)cr[ci][1], ci ? "RTBuddy code" : "AppleANS2NVMeController code"); uint64_t rs = 0, rl = 0; int nr = 0; for (uint64_t pc = cr[ci][0]; pc <= cr[ci][1] && nr < 140; pc += 4) { uint64_t co = pc - c->cov_base; int ex = pc < cr[ci][1] && co < c->cov_len && (c->cov[co >> 5] & (1u << ((co >> 2) & 7))); if (ex) { if (!rs) rs = pc; rl = pc; } else if (rs) { fprintf(stderr, " %llx-%llx", (unsigned long long)rs & 0xfffffffull, (unsigned long long)rl & 0xfffffffull); rs = 0; nr++; } } fprintf(stderr, "\n"); } }
      fprintf(stderr, "[ans-trace] 0.4.417 accesses to ans / sart-ans blocks (r|w physical+offset=value@pc, xN = N identical repeats):%s\n", a64m_ans_trace());
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
      { /* 0.4.291 diagnostic: class name of the SecureRoot registrar: OSMetaClass 0xfffffff009156868, +0x18 = className OSSymbol whose C string pointer is at +0x10; +0x10 = superclass metaclass (same layout) */
        A64 *c9 = c; uint64_t pa9; uint8_t *q9; static const uint64_t mc[2] = { 0xfffffff009156868ull, 0 }; uint64_t cur = mc[0];
        for (int lvl = 0; lvl < 6 && cur > 0xfffffff000000000ull; lvl++) { uint64_t sy = 0, sp = 0, cs = 0; char nb[64]; int nn = 0;
          if (a64_virt_to_phys(c9, cur + 0x18, &pa9) == 0 && (q9 = a64m_ram_at(m, pa9, 8))) memcpy(&sy, q9, 8);
          if (a64_virt_to_phys(c9, cur + 0x10, &pa9) == 0 && (q9 = a64m_ram_at(m, pa9, 8))) memcpy(&sp, q9, 8);
          if (sy && a64_virt_to_phys(c9, sy + 0x10, &pa9) == 0 && (q9 = a64m_ram_at(m, pa9, 8))) memcpy(&cs, q9, 8);
          for (int k = 0; cs && k < 60; k++) { uint8_t ch = 0; if (a64_virt_to_phys(c9, cs + (uint64_t)k, &pa9) == 0 && (q9 = a64m_ram_at(m, pa9, 1))) ch = *q9; else break; if (!ch) break; nb[nn++] = (ch >= 32 && ch < 127) ? (char)ch : '?'; } nb[nn] = 0;
          fprintf(stderr, "[meta] level %d metaclass %llx className OSSymbol %llx string@%llx \"%s\" super %llx\n", lvl, (unsigned long long)cur, (unsigned long long)sy, (unsigned long long)cs, nb, (unsigned long long)sp);
          cur = sp; } }
      { /* 0.4.287: slot called by the SecureRoot registering function at 0x83d21c4 (cbz w0 -> early exit): [0xfffffff0078917d0] (blraa); print raw and low-48-bit target */
        A64 *c8 = c; uint64_t pa8, v8 = 0; uint8_t *q8; for (uint64_t sl = 0xfffffff0078917c0ull; sl <= 0xfffffff0078917e0ull; sl += 8) { v8 = 0; if (a64_virt_to_phys(c8, sl, &pa8) == 0 && (q8 = a64m_ram_at(m, pa8, 8))) memcpy(&v8, q8, 8); fprintf(stderr, "[slot] %llx = %016llx -> %llx\n", (unsigned long long)sl, (unsigned long long)v8, (unsigned long long)(0xffff000000000000ull | (v8 & 0xffffffffffffull))); } }
      { /* 0.4.282: the polling loop reads [[x19+0x1a8] + reg] (readReg32 at 0x8483e74); print that MMIO base and its physical address */
        A64 *c7 = c; uint64_t o = c7->x[19], pa7, v7 = 0; uint8_t *q7; fprintf(stderr, "[mmio] x19=%llx", (unsigned long long)o);
        for (int w = 0x190; w <= 0x1b0; w += 8) { v7 = 0; if (a64_virt_to_phys(c7, o + (uint64_t)w, &pa7) == 0 && (q7 = a64m_ram_at(m, pa7, 8))) memcpy(&v7, q7, 8); uint64_t pb = 0; int ok = a64_virt_to_phys(c7, v7, &pb) == 0; fprintf(stderr, " [+%x]=%llx(pa %llx%s)", w, (unsigned long long)v7, (unsigned long long)pb, ok ? "" : "?"); } fprintf(stderr, "\n"); }
      fprintf(stderr, "[cpu] x9..x29:"); for (int k = 9; k < 30; k++) fprintf(stderr, " %llx", (unsigned long long)c->x[k]); fprintf(stderr, "\n");   /* 0.4.281 */
      fprintf(stderr, "[cpu] x0..x8:"); for (int k = 0; k < 9; k++) fprintf(stderr, " %llx", (unsigned long long)c->x[k]); fprintf(stderr, " lr=%llx ttbr0=%llx ttbr1=%llx tpidr_el0=%llx\n", (unsigned long long)c->x[30], (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->tpidr_el0); }
    return 0;
}
