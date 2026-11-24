#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#define LOG_CAP (1u << 20)
static int g_quiet_exc; static int g_diag_dumped;

typedef struct { uint64_t addr; uint32_t enable, frame; } Fb;
typedef struct { uint32_t x, y, state, seq, ctrl, status; } Touch;

struct A64Machine {
    A64MachineKind kind;
    A64 cpu;
    uint8_t *ram; uint64_t ram_base, ram_size;
    volatile int stop;
    A64MState state;
    pthread_mutex_t lock;
    char *slog[3]; size_t slen[3];
    /* 0.4.137: collapsing of identical consecutive GUEST output lines (the kernel's AppleT8015TempSensor timeout line fills the 1 MiB log): complete lines are held until the newline */
    bool collapse; char gl_cur[600], gl_prev[600]; size_t gl_n, gl_pn; bool gl_pv; unsigned gl_rep;
    Fb fb; Touch touch;
    struct { uint64_t pa; uint32_t w, h, rowbytes; uint64_t sum; int64_t frame; } vid;   /* boot_args.Video framebuffer (IPSW attempt), read-only for the UI */
    uint64_t entry;
    bool reported_end; bool stop_dumped;   /* 0.4.71: the full stop report was already printed by the stuck-loop path */
    bool raw, attempt;
    unsigned impl_n; unsigned impl_key[96]; uint64_t impl_val[96]; unsigned char impl_seen[96]; uint64_t pmin, pmax, calm_exc; unsigned calm;   /* stuck-loop detector (attempt machines only) */
    uint64_t virt_base, phys_base;
    struct S5l { void *m; uint32_t r[0x40]; } s5l[3]; unsigned s5l_n;
    struct { uint64_t hdr_pa, buf_pa; uint32_t size, last; uint64_t next; int found; } mb;   /* 0.4.66: kernel message buffer (msgbuf) mirror */
};

/* ----------------------------------------------------------------- log */
/* Three streams: ALL (chronological, what tests read), GUEST (only what the guest itself wrote: UART / HLE output)
 * and EMU (only what the emulator says about itself). */
static void stream_add(A64Machine *m, int s, const char *p, size_t n) {
    const size_t cap = LOG_CAP - 32;
    if (m->slen[s] >= cap) return;
    if (n > cap - m->slen[s]) n = cap - m->slen[s];
    memcpy(m->slog[s] + m->slen[s], p, n); m->slen[s] += n;
    if (m->slen[s] >= cap) { const char *t = "\n[log truncated]\n"; memcpy(m->slog[s] + m->slen[s], t, strlen(t)); m->slen[s] += strlen(t); }
}
static void guest_emit(A64Machine *m, const char *p, size_t n) { stream_add(m, 0, p, n); stream_add(m, 1, p, n); }
static void guest_note(A64Machine *m) {   /* lock held */
    if (!m->gl_rep) return;
    char t[80]; int k = snprintf(t, sizeof t, "[last line repeated %u more time%s]\n", m->gl_rep, m->gl_rep == 1 ? "" : "s");
    guest_emit(m, t, (size_t)k); m->gl_rep = 0;
}
static void pmp_trace_line(A64Machine *m);
static void guest_line_done(A64Machine *m, bool nl) {   /* lock held; gl_cur holds one complete line (nl) or a part of a very long one */
    if (nl && m->gl_n < 600) { m->gl_cur[m->gl_n] = 0; if (strstr(m->gl_cur, "PMP") || strstr(m->gl_cur, "Endpoint")) pmp_trace_line(m); }
    if (nl && m->gl_pv && m->gl_n == m->gl_pn && !memcmp(m->gl_cur, m->gl_prev, m->gl_n)) { if (++m->gl_rep >= 256) guest_note(m); m->gl_n = 0; return; }
    guest_note(m); guest_emit(m, m->gl_cur, m->gl_n);
    if (nl) { memcpy(m->gl_prev, m->gl_cur, m->gl_n); m->gl_pn = m->gl_n; m->gl_pv = true; } else m->gl_pv = false;
    m->gl_n = 0;
}
static char g_pmp_tr[6000]; static size_t g_pmp_trn; static int g_pmp_trc;
const char *a64m_pmp_print_trace(void) { return g_pmp_tr; }
static void pmp_trace_line(A64Machine *m) {
    if (g_pmp_trc >= 14 || g_pmp_trn > sizeof g_pmp_tr - 400) return; g_pmp_trc++;
    g_pmp_trn += (size_t)snprintf(g_pmp_tr + g_pmp_trn, sizeof g_pmp_tr - g_pmp_trn, "[pmpprint] \"%.60s\" pc=%llx lr=%llx chain:", m->gl_cur, (unsigned long long)(m->cpu.pc & 0xfffffffffull), (unsigned long long)(m->cpu.x[30] & 0xfffffffffull));
    uint64_t fp = m->cpu.x[29];
    for (int d = 0; d < 14 && fp > 0xffffffe000000000ull; d++) { uint64_t pa, nf, lr; uint8_t *q;
        if (a64_virt_to_phys(&m->cpu, fp, &pa) || !(q = a64m_ram_at(m, pa, 16))) break; memcpy(&nf, q, 8); memcpy(&lr, q + 8, 8);
        g_pmp_trn += (size_t)snprintf(g_pmp_tr + g_pmp_trn, sizeof g_pmp_tr - g_pmp_trn, " %llx", (unsigned long long)(lr & 0xfffffffffull)); fp = nf; }
    g_pmp_trn += (size_t)snprintf(g_pmp_tr + g_pmp_trn, sizeof g_pmp_tr - g_pmp_trn, "\n");
}
static void guest_flush(A64Machine *m) { if (m->gl_n) guest_line_done(m, false); guest_note(m); }   /* lock held */
static void log_raw_k(A64Machine *m, const char *s, size_t n, int kind) {   /* kind: 1 guest, 2 emulator */
    pthread_mutex_lock(&m->lock);
    if (kind == 1 && m->collapse) {
        for (size_t i = 0; i < n; i++) { m->gl_cur[m->gl_n++] = s[i]; if (s[i] == '\n') guest_line_done(m, true); else if (m->gl_n >= sizeof m->gl_cur) guest_line_done(m, false); }
    } else {
        if (m->collapse) guest_flush(m);   /* keep the chronological order: an emulator message first ends the pending guest line */
        stream_add(m, 0, s, n); stream_add(m, kind, s, n);
    }
    pthread_mutex_unlock(&m->lock);
}
void a64m_log_collapse(A64Machine *m, bool on) { pthread_mutex_lock(&m->lock); if (!on) guest_flush(m); m->collapse = on; pthread_mutex_unlock(&m->lock); }
void a64m_log_flush(A64Machine *m) { pthread_mutex_lock(&m->lock); if (m->collapse) guest_flush(m); pthread_mutex_unlock(&m->lock); }
static void log_raw(A64Machine *m, const char *s, size_t n) { log_raw_k(m, s, n, 1); }   /* guest output */
void a64m_log(A64Machine *m, const char *fmt, ...) {
    char b[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n < 0) return; if ((size_t)n >= sizeof b) n = sizeof b - 1;
    log_raw_k(m, b, (size_t)n, 2);
}
size_t a64m_log_read_kind(A64Machine *m, int kind, size_t off, char *buf, size_t cap) {
    if (kind < 0 || kind > 2) return 0;
    pthread_mutex_lock(&m->lock);
    size_t n = off < m->slen[kind] ? m->slen[kind] - off : 0; if (n > cap) n = cap;
    if (n) memcpy(buf, m->slog[kind] + off, n);
    pthread_mutex_unlock(&m->lock);
    return n;
}
size_t a64m_log_read(A64Machine *m, size_t off, char *buf, size_t cap) { return a64m_log_read_kind(m, 0, off, buf, cap); }
size_t a64m_log_size(A64Machine *m) { pthread_mutex_lock(&m->lock); size_t n = m->slen[0]; pthread_mutex_unlock(&m->lock); return n; }

/* ------------------------------------------------------------- devices */
static void update_irq(A64Machine *m) {
    bool line = (m->touch.ctrl & 1) && (m->touch.status & 1);
    __atomic_store_n(&m->cpu.irq_line, line, __ATOMIC_RELEASE);
}
static uint64_t uart_r(void *u, uint64_t off, unsigned sz) { (void)u; (void)sz; return off == 0x18 ? 0x90 : 0; }
static void uart_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    (void)sz; if (off == 0) { char ch = (char)v; log_raw((A64Machine *)u, &ch, 1); }
}

/* Apple S5L (Samsung-style) UART, as the XNU serial driver uses it (offsets as I know them, unverified): ULCON 0x00, UCON 0x04, UFCON 0x08, UMCON 0x0c,
 * UTRSTAT 0x10 (bit0 rx data, bit1 tx buffer empty, bit2 tx shifter empty), UFSTAT 0x18, UTXH 0x20, URXH 0x24. MODELLED: the transmitter is always empty,
 * nothing is ever received, every other register reads back what was last written. Bytes written to UTXH go to the guest log (the kernel's own console). */
static uint64_t s5l_r(void *u, uint64_t off, unsigned sz) {
    struct S5l *d = u; (void)sz;
    switch (off) { case 0x10: return 0x6; case 0x18: return 0; case 0x1c: return 0; case 0x24: return 0; default: return off < sizeof d->r ? d->r[off >> 2] : 0; }
}
static void s5l_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    struct S5l *d = u; (void)sz;
    if (off == 0x20) { char ch = (char)v; log_raw((A64Machine *)d->m, &ch, 1); return; }
    if (off < sizeof d->r) d->r[off >> 2] = (uint32_t)v;
}
int a64m_add_s5l_uart(A64Machine *m, uint64_t pa, uint64_t size) {
    if (m->s5l_n >= 3) return -1;
    struct S5l *d = &m->s5l[m->s5l_n]; d->m = m;
    static const char *names[3] = { "s5l-uart-a", "s5l-uart-b", "s5l-uart-c" };
    A64Dev dev = { pa, size ? size : 0x4000, s5l_r, s5l_w, d, names[m->s5l_n] };
    if (a64_add_device(&m->cpu, &dev)) return -1;
    m->s5l_n++; return 0;
}
/* MODELLED placeholder for an SoC register block whose real behaviour is unknown: reads return 0, writes are ignored, and every access (first 40) is logged so the
 * stop report shows exactly what the kernel touches. This is an assumption, not hardware knowledge. */
static struct Unk { A64Machine *m; int is_i2c, is_dart, is_spmi, is_aic; uint32_t sp_fifo[16]; unsigned sp_n, sp_irq; uint32_t sp_en[8]; uint32_t aic_pend[16], aic_mask[16]; uint16_t aw_off[128]; uint32_t aw_or[128], aw_n[128]; int aw_cnt; uint32_t i2c_st; uint64_t ts_base[16]; int ts_n; uint64_t ts_off[160], ts_val[160]; int ts_rn; unsigned n, cap; char label[44]; uint64_t pa; uint64_t rv_off[16], rv_val[16]; int rv_n; unsigned reads, writes; uint64_t last_off; int rb; uint32_t rbv[1024]; int ps_n; uint64_t ps_off[512]; uint32_t ps_val[512]; int ps; int lk_n; uint64_t lk_w[8], lk_r[8]; uint32_t lk_v[8]; int lk_set[8]; char ac[170]; int asc, asc_mb; } g_unk[360]; static int g_unk_n;
/* 0.4.234 MODELLED (ASSUMPTION, PLACEHOLDER): minimal interrupt controller for the DeviceTree node "aic" (aic,t8020, reg 0x23b100000). Register layout taken from the PUBLIC Linux (Asahi) driver of the first-generation Apple AIC, NOT verified on this chip: EVENT +0x2004 (read: lowest pending unmasked interrupt as (1<<16)|number, acknowledged; 0 = none), MASK_SET +0x4100.., MASK_CLR +0x4180.. (one bit per interrupt, 32 per word), everything starts masked. No priorities, no target CPU, no IPIs, no software interrupts. The CPU's IRQ input is the OR of pending&~mask. */
static uint32_t g_aic_clr[16];   /* 0.4.477 diagnostic: every interrupt number the kernel ever unmasked */
static void aic_update(struct Unk *d) { int any = 0; for (int i = 0; i < 16; i++) if (d->aic_pend[i] & ~d->aic_mask[i]) any = 1; d->m->cpu.irq_line = any ? true : false; }
static struct Unk *aic_of(A64Machine *m);
/* 0.4.96: ring of the last 24 reads from unknown blocks (the memory-controller block at 0x200000000, polled millions of times, is left out) - shows which register was read right before a panic */
static struct { A64Machine *m; char label[44]; uint64_t off, val; } g_rr[24]; static unsigned g_rr_n;
static struct { char w; uint64_t off, v; } g_tsr[60]; static unsigned g_tsn;   /* 0.4.149 diagnostic: ring of the LAST accesses inside any temperature sensor window (offset from the start of the pmgr block) */
static void ts_trace(struct Unk *d, int w, uint64_t off, uint64_t v) { for (int k = 0; k < d->ts_n; k++) { uint64_t r = off - d->ts_base[k]; if (r < 0x34000) { unsigned i = g_tsn++ % 60; g_tsr[i].w = w ? 'w' : 'r'; g_tsr[i].off = off; g_tsr[i].v = v; break; } } }
const char *a64m_tempsensor_trace(void) { static char b[2600]; size_t l = 0; b[0] = 0; unsigned n = g_tsn < 60 ? g_tsn : 60, st = g_tsn < 60 ? 0 : g_tsn % 60; for (unsigned k = 0; k < n && l < sizeof b - 40; k++) { unsigned i = (st + k) % 60; l += (size_t)snprintf(b + l, sizeof b - l, " %c+0x%llx=0x%llx", g_tsr[i].w, (unsigned long long)g_tsr[i].off, (unsigned long long)g_tsr[i].v); } return b; }
/* 0.4.217 diagnostic: ring of the LAST 80 accesses to blocks whose DeviceTree name contains "i2c" (which register starts a transaction, what the poll loop reads) */
static struct { char w, lab[12]; uint64_t off, v; } g_i2r[300]; static unsigned g_i2n;
static void i2_trace(struct Unk *d, int w, uint64_t off, uint64_t v) { if (!strstr(d->label, "aes")) return; unsigned i = g_i2n++ % 300; g_i2r[i].w = (char)(w ? 'w' : 'r'); snprintf(g_i2r[i].lab, sizeof g_i2r[i].lab, "%.10s", d->label + 1); g_i2r[i].off = off; g_i2r[i].v = v; }
const char *a64m_i2c_trace(void) { static char b[13000]; size_t l = 0; b[0] = 0; unsigned n = g_i2n < 300 ? g_i2n : 300, st = g_i2n < 300 ? 0 : g_i2n % 300; for (unsigned k = 0; k < n && l < sizeof b - 50; k++) { unsigned i = (st + k) % 300; l += (size_t)snprintf(b + l, sizeof b - l, " %s:%c+0x%llx=0x%llx", g_i2r[i].lab, g_i2r[i].w, (unsigned long long)g_i2r[i].off, (unsigned long long)g_i2r[i].v); } return b; }
/* 0.4.417 diagnostic: trace of the accesses to the ANS2 / sart-ans blocks (the NAND/NVMe coprocessor path): first 700 distinct consecutive accesses with PC; identical repeats (polling) are counted, not stored */
static struct { char w; uint64_t pa, off, v, pc; unsigned rep; } g_an[1200]; static unsigned g_ann; static uint64_t g_an_pa[24]; static unsigned g_an_c[24];   /* 0.4.472: at most 48 stored entries per block */
static void an_trace(struct Unk *d, int w, uint64_t off, uint64_t v) { if (!strstr(d->label, "ans") && !strstr(d->label, "pmp")) return; if (g_ann && g_an[g_ann - 1].w == (char)w && g_an[g_ann - 1].pa == d->pa && g_an[g_ann - 1].off == off && g_an[g_ann - 1].v == v) { g_an[g_ann - 1].rep++; return; } if (g_ann >= 1200) return; { int q = 0; while (q < 24 && g_an_c[q] && g_an_pa[q] != d->pa) q++; if (q >= 24) return; if (!g_an_c[q]) { g_an_pa[q] = d->pa; } if (g_an_c[q] >= 48) return; g_an_c[q]++; } g_an[g_ann].w = (char)(w ? 'w' : 'r'); g_an[g_ann].pa = d->pa; g_an[g_ann].off = off; g_an[g_ann].v = v; g_an[g_ann].pc = d->m->cpu.pc; g_an[g_ann].rep = 0; g_ann++; }
const char *a64m_ans_trace(void) { static char b[100000]; size_t l = 0; b[0] = 0; for (unsigned k = 0; k < g_ann && l < sizeof b - 90; k++) { l += (size_t)snprintf(b + l, sizeof b - l, " %c%llx+%llx=%llx@%llx", g_an[k].w, (unsigned long long)g_an[k].pa, (unsigned long long)g_an[k].off, (unsigned long long)g_an[k].v, (unsigned long long)g_an[k].pc & 0xfffffffffull); if (g_an[k].rep) l += (size_t)snprintf(b + l, sizeof b - l, "x%u", g_an[k].rep + 1); } return b; }

/* 0.4.478 MODELLED (ASSUMPTION): the IOP side of an Apple "ASC" mailbox, used for coprocessors whose firmware (embedded in the kernelcache) we do NOT execute (PMP first). NOT real firmware: a scripted RTKit management endpoint (ep0) after the public Asahi Linux RTKit documentation. Register layout READ FROM THE KERNEL's own code (0.4.474): mailbox block = ASC base + 0x8000; +0x8108 A2I control, +0x810c I2A control (bit0 enable, bit16 full, bit17 empty, bits 20..23 entry count), +0x8800/+0x8808 A2I_SEND (16 bytes), +0x8830/+0x8838 I2A_RECV (16 bytes, pop on the 2nd word; its high half carries the count field, the kernel loops on it). CPU_CONTROL = ASC base + 0x44, bit 4 = run (Asahi docs + the kernel writes 0x10 there). The firmware's own endpoint list / protocol version are UNKNOWN here: the values used are PLACEHOLDERS and every message is logged. */
static struct Asc { int used; A64Machine *m; uint64_t cpu_pa; unsigned irq; char tag[8]; int run, en_a, en_i; unsigned qn; uint64_t q[16][2]; uint64_t a0, a1; unsigned stage; uint64_t epmap; unsigned app_stage; struct { char dir; uint64_t m0, m1; } lg[96]; unsigned lgn; } g_asc[4];
static void asc_log(struct Asc *a, char dir, uint64_t m0, uint64_t m1) { if (a->lgn < 96) { a->lg[a->lgn].dir = dir; a->lg[a->lgn].m0 = m0; a->lg[a->lgn].m1 = m1; a->lgn++; } }
static void asc_push(struct Asc *a, uint64_t m0, uint64_t m1) { if (a->qn < 16) { a->q[a->qn][0] = m0; a->q[a->qn][1] = m1; a->qn++; asc_log(a, 'I', m0, m1); a64m_aic_raise(a->m, a->irq); } }
static void asc_ap_msg(struct Asc *a, uint64_t m0, uint64_t m1) {
    asc_log(a, 'A', m0, m1);
    unsigned ep = (unsigned)(m1 & 0xff), ty = (unsigned)((m0 >> 52) & 0xff);
    if (ep == 0 && ty == 2 && a->stage == 1 && (m0 & 0xffffffffu) == 0x0badffffu) { a->stage = 3; }   /* the kernel's own answer to an unacceptable version range (0.4.478 log: 0x0badffff) */
    else if (ep == 0 && ty == 2 && a->stage == 1) { a->stage = 2; asc_push(a, (8ull << 52) | (a->epmap ? a->epmap : 0xdfu), 0); }   /* 0.4.531: ep0..4 plus ep6, ep7. DERIVED FROM KERNEL CODE (0x82edd1c, name builder): an endpoint index k >= 6 becomes the nub '<role>Endpoint<k-5>' (k=6 -> 'Endpoint1'); the plist matches ApplePMP on PMPEndpoint1 / PMPEndpoint2. Which endpoints the real PMP firmware advertises is UNKNOWN: ASSUMPTION */   /* 0.4.525: the kernel's EPMAP handler (0x82f4858) takes ONE message whose low 52 bits are the endpoint bitmap (bit 0 must be set, REQUIRE status==4, a second EPMAP panics). EXPERIMENT/PLACEHOLDER: ep0..4 plus 32,33 (0x20,0x21: guess for PMPEndpoint1/2) */   /* 0.4.523 EXPERIMENT: group 0 is no longer flagged LAST; group 1 follows (below) */   /* HELLO_ACK -> EPMAP, last: ep0 + the standard RTKit system endpoints 1..4 (crashlog, syslog, debug, ioreport per the public Asahi docs): PLACEHOLDER, the real PMP firmware's list is UNKNOWN. 0.4.484: with {ep0} only the kernel answered DONE and then stayed silent */
    else if (ep == 0 && ty == 8 && a->stage == 2) { a->stage = 4; asc_push(a, (7ull << 52) | 0x20u, 0); }   /* AP's EPMAP reply (DONE) seen -> EXPERIMENT 0.4.487: announce 'IOP power state ON (0x20)' unsolicited (the 0.4.486 log shows the kernel started endpoints 1..4 and then waited; the kernel's power-state code (0x82f4b18) accepts state 0x20); PLACEHOLDER */
    else if (ep == 0 && ty == 0xb) asc_push(a, (0xbull << 52) | (m0 & 0xffffffffu), 0);   /* AP power state (type 0xb) -> echo it back as type 0xb: the kernel's dispatcher (0x82f3e84: jump table on bits 55:52) sends type 0xb to the handler at 0x82f3f20, which stores the low 32 bits at object+232 and wakes the sleeper (_setPowerStateGuardEntryGated). ASSUMPTION: this is the acknowledgement it waits for */
    else if (ep == 0 && ty == 5 && !strcmp(a->tag, "PMP") && ((m0 >> 32) & 0xff) == 6 && a->app_stage == 0) { a->app_stage = 1; asc_push(a, 0, 6); }   /* 0.4.593 MODELLED (ASSUMPTION, PLACEHOLDER; NOT the PMP firmware): the kernel started endpoint 6 (PMPEndpoint1). ApplePMP's message handler (0x89a6a2c, jump table 0x89a74c4, type = bits 55:52) treats a type-0 message as "IOP describes its shared memory": size in 4K pages = bits 45:20, bits 10:8 = number of extra arrays, bit 11 = flag, bits 3:0 = version-like field. The REAL values are UNKNOWN (they come from the firmware we do not execute), so this sends the minimum the code accepts: type 0 with ALL fields 0 (size 0 = no shared memory, the code skips the allocation). Real PMP would report non-zero values; features depending on them will not work. */
    else if (ep == 6 && !strcmp(a->tag, "PMP") && a->app_stage == 1 && ((m0 >> 52) & 0xf) == 1) { a->app_stage = 2; asc_push(a, 2ull << 52, 6); }   /* 0.4.593 MODELLED (ASSUMPTION): the kernel's answer to type 0 is a type-1 message (carrying the physical address of its own buffer; logged). The handler moves ApplePMP from state 1 to state 2 on a type-2 message (0x89a7094: state must be 1), which is what ApplePMGR waits for; the model sends exactly that: type 2, no payload. */
    else if (ep == 6 && !strcmp(a->tag, "PMP") && a->app_stage == 2 && ((m0 >> 52) & 0xf) == 9) { uint64_t r = (10ull << 52) | (m0 & (0xfull << 56)) | (((((m0 >> 47) & 0x1f) + 1) & 0x1f) << 47) | (m0 & (7ull << 44)) | (m0 & (0xfull << 40)); asc_push(a, r, 6); }   /* 0.4.595 MODELLED (ASSUMPTION; PMP firmware NOT executed): ApplePMP's request sender (0x89a7f58..0x89a8030) builds type 9 = {bits 59:56 own id, 51:47 command (valid: 0,2,8,10,12), 46:44 slot, 43:40 arg, 39:20 and 19:0 payload} and waits for a type-10 reply (0x89a7180 stores the low 52 bits in the slot and wakes the sender), then REQUIRES reply.command == command+1, reply.slot == slot, reply.arg == arg (0x89a8034..0x89a80ac). The model echoes id/slot/arg, answers command+1 and sets the payload to 0 = "no data" (REAL payloads UNKNOWN; for commands 10/12 the sender turns an all-zero payload into the error 0xe00002bc). */
    else if (ep == 0 && ty == 6) asc_push(a, (7ull << 52) | (m0 & 0xffffu), 0);   /* AP asks for an IOP power state -> acknowledge the same state (Asahi docs: type 6 request, type 7 ack carrying the state in bits 15:0) */
}
static void asc_cpu_w(struct Asc *a, uint64_t v) { int run = (v >> 4) & 1; if (run && !a->run && a->stage == 0) { a->stage = 1; asc_push(a, (1ull << 52) | (10ull << 16) | 10u, 0); } a->run = run; }   /* RUN -> HELLO, max version 10 = the ONLY value the kernel's HELLO handler accepts (code at 0x82f4754: ubfx(msg,16,16) compared with 10; any other value is answered with 0x0badffff, built at 0x82f4814); min=10 is a PLACEHOLDER (the kernel does not compare it there) */
int a64m_asc_add(A64Machine *m, uint64_t cpu_pa, unsigned irq, const char *tag) {
    int ix = -1; for (int k = 0; k < 4; k++) if (g_asc[k].used && g_asc[k].cpu_pa == cpu_pa) { ix = k; break; }   /* the same coprocessor of a new machine replaces the old model (tests / reboot) */
    for (int k = 0; ix < 0 && k < 4; k++) if (!g_asc[k].used) ix = k; if (ix < 0) return -1;
    int n = 0; for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m && g_unk[k].pa == cpu_pa) { g_unk[k].asc = ix + 1; g_unk[k].asc_mb = 0; n++; } else if (g_unk[k].m == m && g_unk[k].pa == cpu_pa + 0x8000) { g_unk[k].asc = ix + 1; g_unk[k].asc_mb = 1; n++; }
    if (n < 2) return -2;
    memset(&g_asc[ix], 0, sizeof g_asc[ix]); g_asc[ix].used = 1; g_asc[ix].m = m; g_asc[ix].cpu_pa = cpu_pa; g_asc[ix].irq = irq; g_asc[ix].epmap = 0; snprintf(g_asc[ix].tag, sizeof g_asc[ix].tag, "%s", tag); return ix; }
int a64m_asc_set_epmap(uint64_t cpu_pa, uint64_t map) { for (int k = 0; k < 4; k++) if (g_asc[k].used && g_asc[k].cpu_pa == cpu_pa) { g_asc[k].epmap = map & 0xfffffffffffffull; return 0; } return -1; }   /* 0.4.532: endpoint bitmap of the modelled EPMAP (low 52 bits; bit 0 required by the kernel) */
const char *a64m_asc_log(void) { static char b[9000]; size_t l = 0; b[0] = 0; for (int k = 0; k < 4; k++) if (g_asc[k].used) { l += (size_t)snprintf(b + l, sizeof b - l, "[asc-%s] stage=%u run=%d queued=%u messages (I = IOP->AP model, A = AP->IOP seen from the kernel; msg0 msg1):", g_asc[k].tag, g_asc[k].stage, g_asc[k].run, g_asc[k].qn); for (unsigned q = 0; q < g_asc[k].lgn && l < sizeof b - 120; q++) l += (size_t)snprintf(b + l, sizeof b - l, " %c:%llx,%llx", g_asc[k].lg[q].dir, (unsigned long long)g_asc[k].lg[q].m0, (unsigned long long)g_asc[k].lg[q].m1); l += (size_t)snprintf(b + l, sizeof b - l, "\n"); } return b; }
static int asc_r(struct Unk *d, uint64_t off, unsigned sz, uint64_t *val) {
    struct Asc *a = &g_asc[d->asc - 1];
    if (!d->asc_mb) return 0;
    if (off == 0x108) { *val = (uint64_t)a->en_a | 0x20000u; return 1; }   /* A2I: always "empty": the model consumes AP messages at once */
    if (off == 0x10c) { *val = (uint64_t)a->en_i | (a->qn ? ((uint64_t)(a->qn > 15 ? 15 : a->qn) << 20) : 0x20000u); return 1; }
    if (off == 0x830 || off == 0x834) { uint64_t m0 = a->qn ? a->q[0][0] : 0; *val = off == 0x834 ? (m0 >> 32) : (sz == 8 ? m0 : (m0 & 0xffffffffu)); return 1; }
    if (off == 0x838 || off == 0x83c) { uint64_t m1 = a->qn ? a->q[0][1] & 0xffffffffu : 0; uint64_t hi = a->qn ? ((uint64_t)(a->qn > 15 ? 15 : a->qn) << 20) : 0; uint64_t full = m1 | (hi << 32); *val = off == 0x838 ? (sz == 8 ? full : (full & 0xffffffffu)) : (full >> 32);
        if (a->qn && (sz == 8 || off == 0x83c)) { for (unsigned q = 1; q < a->qn; q++) { a->q[q - 1][0] = a->q[q][0]; a->q[q - 1][1] = a->q[q][1]; } a->qn--; }
        return 1; }
    return 0; }
static void asc_w(struct Unk *d, uint64_t off, unsigned sz, uint64_t v) {
    struct Asc *a = &g_asc[d->asc - 1];
    if (!d->asc_mb) { if (off == 0x44) asc_cpu_w(a, v); return; }
    if (off == 0x108) a->en_a = (int)(v & 1);
    else if (off == 0x10c) a->en_i = (int)(v & 1);
    else if (off == 0x800) a->a0 = sz == 8 ? v : ((a->a0 & ~0xffffffffull) | (v & 0xffffffffu));
    else if (off == 0x804) a->a0 = (a->a0 & 0xffffffffull) | ((v & 0xffffffffu) << 32);
    else if (off == 0x808 && sz == 8) asc_ap_msg(a, a->a0, v & 0xffffffffu);
    else if (off == 0x808) a->a1 = v & 0xffffffffu;
    else if (off == 0x80c) asc_ap_msg(a, a->a0, a->a1); }   /* 32-bit accesses: the message is complete with the last word */
static uint64_t unk_r(void *u, uint64_t off, unsigned sz) {
    struct Unk *d = u; uint64_t val = 0; int fixed = 0; d->reads++; d->last_off = off;
    if (d->asc && asc_r(d, off, sz, &val)) { fixed = 9; }   /* 0.4.478 */
    for (int k = 0; k < d->rv_n; k++) if (d->rv_off[k] == off) { val = d->rv_val[k]; fixed = 1; }
    for (int k = 0; !fixed && k < d->ts_rn; k++) if (d->ts_off[k] == off) { val = d->ts_val[k]; fixed = 5; }   /* 0.4.150 MODELLED temperature sensor registers, see a64m_unknown_ts_read */
    if (!fixed && d->is_dart && off == 0) { val = 0x0e000000u; fixed = 6; }   /* 0.4.224 MODELLED (ASSUMPTION, PLACEHOLDER): DART register +0x0, bits 27:24 = page-size shift. Source: AppleT8020DART.cpp:1918 compares this 4-bit field with the configured page shift and prints actual = 1<<field; config was 0x4000 (16 KiB) so the field must be 14. Other bits of the register are UNKNOWN and read 0 */
    if (!fixed && d->is_aic && off == 0x2004) { val = 0; for (unsigned i = 0; i < 512 && !val; i++) if ((d->aic_pend[i >> 5] & ~d->aic_mask[i >> 5]) >> (i & 31) & 1) { d->aic_pend[i >> 5] &= ~(1u << (i & 31)); val = 0x10000u | i; } aic_update(d); fixed = 8; }   /* 0.4.234 */
    if (!fixed && d->is_spmi && off >= 0x720 && off < 0x740) { val = d->sp_en[(off - 0x720) >> 2]; fixed = 7; }   /* 0.4.240 MODELLED (ASSUMPTION): interrupt-enable words +0x720.. read back what the driver wrote. Source: the SPMI interrupt handler (0.4.239 trace) reads +0x720+4i (enable) and +0x760+4i (status) for i=0..6 and handles status&enable; it saw enable 0 for word 6 because we returned 0 */
    if (!fixed && d->is_spmi && off == 0x778) { val = d->sp_irq; fixed = 7; }   /* 0.4.234 MODELLED (ASSUMPTION): per-interrupt status words at +0x60+4*(irq/32) of the controller; interrupt 192 (the first DeviceTree interrupt of "spmi", the one AppleSPMI waits for: object field +0x168 = 0xc0) = word 6 bit 0 = +0x78. Source: AppleSPMI polled-mode code (0x89829bc) */
    if (!fixed && d->is_spmi && (off == 0x700 || off == 0xd00)) { val = d->sp_n ? 0 : 0x01000000u; fixed = 7; }   /* 0.4.229 MODELLED (ASSUMPTION, PLACEHOLDER): SPMI controller status +0x0 of each controller (+0x700, +0xd00 in the block): bit 24 = RX FIFO empty. Source: AppleSPMI keeps reading with bit 24 clear and gives up after 10 rounds ("spmi timeout"); bit 24 is also the failure test in its poll routine */
    if (!fixed && d->is_spmi && (off == 0x708 || off == 0xd08)) { if (d->sp_n) { val = d->sp_fifo[0]; for (unsigned q = 1; q < d->sp_n; q++) d->sp_fifo[q - 1] = d->sp_fifo[q]; d->sp_n--; } fixed = 7; }   /* 0.4.229 MODELLED: +0x8 = response FIFO pop; the response of every command is the command word echoed (ASSUMPTION: real response framing UNKNOWN; no PMU data is invented, a read returns "the command" not a register value) */
    if (!fixed && d->is_i2c && off == 0x14) { val = d->i2c_st | 0x01000000u; fixed = 6; }   /* 0.4.219 MODELLED (ASSUMPTION): I2C controller of an idle, empty bus: status bit 24 reads 1. Source: _checkBusStatus asserts when bit 24 is 0 ("SCL is stuck low" side); it is read as the SCL line level */
    if (!fixed && d->rb && off < sizeof d->rbv) { val = d->rbv[off >> 2]; fixed = 2; }   /* 0.4.61: read-back block */
    if (!fixed) for (int k = 0; k < d->lk_n; k++) if (d->lk_r[k] == off && d->lk_set[k]) { val = (d->lk_v[k] << 10) | (d->lk_v[k] << 5); fixed = 4; }   /* 0.4.135 mode link, see a64m_unknown_set_modelink */
    if (!fixed && d->ps) for (int k = 0; k < d->ps_n; k++) if (d->ps_off[k] == off) { uint32_t w = d->ps_val[k]; val = (w & ~0xf0u) | ((w & 0xfu) << 4); fixed = 3; }   /* 0.4.110 power-state mirror, see a64m_unknown_set_psmirror */
    if (d->n < d->cap && d->n < 8) { size_t l = strlen(d->ac); d->n++; if (l < sizeof d->ac - 40) snprintf(d->ac + l, sizeof d->ac - l, " r+0x%llx=0x%llx", (unsigned long long)off, (unsigned long long)val); }   /* 0.4.71: first accesses are kept for the stop report instead of one log line each */
    if (d->ts_n) ts_trace(d, 0, off, val);
    i2_trace(d, 0, off, val); an_trace(d, 0, off, val);
    if (d->pa != 0x200000000ull) { unsigned i = g_rr_n++ % 24; g_rr[i].m = d->m; snprintf(g_rr[i].label, sizeof g_rr[i].label, "%s@%llx", d->label[0] ? d->label : "'?'", (unsigned long long)d->pa); g_rr[i].off = off; g_rr[i].val = val; }
    return val;
}
static uint8_t g_pmpfw[0x40000]; static size_t g_pmpfw_hi;   /* 0.4.536 capture only: bytes the kernel writes into the PMP SRAM window (the PMP firmware image it copies there); reads are NOT affected */
const uint8_t *a64m_pmp_fw(size_t *n) { if (n) *n = g_pmpfw_hi; return g_pmpfw; }
static void unk_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    struct Unk *d = u; if (d->pa == 0x23b500000ull && off + sz <= sizeof g_pmpfw && sz <= 8) { memcpy(g_pmpfw + off, &v, sz); if (off + sz > g_pmpfw_hi) g_pmpfw_hi = off + sz; }
    d->writes++; d->last_off = off; if (d->asc) asc_w(d, off, sz, v);   /* 0.4.478 */ if (d->ts_n) ts_trace(d, 1, off, v); i2_trace(d, 1, off, v); an_trace(d, 1, off, v);
    if (d->is_aic && !(off >= 0x3000 && off < 0x4000 && !v)) { int k = 0; while (k < d->aw_cnt && d->aw_off[k] != (uint16_t)off) k++; if (k == d->aw_cnt && k < 128) { d->aw_off[k] = (uint16_t)off; d->aw_cnt++; } if (k < 128) { d->aw_or[k] |= (uint32_t)v; d->aw_n[k]++; } }   /* 0.4.235 diagnostic: distinct written offsets with the OR of the values */
    if (d->is_aic) { if (off >= 0x4100 && off < 0x4140) d->aic_mask[(off - 0x4100) >> 2] |= (uint32_t)v; else if (off >= 0x4180 && off < 0x41c0) { d->aic_mask[(off - 0x4180) >> 2] &= ~(uint32_t)v; g_aic_clr[(off - 0x4180) >> 2] |= (uint32_t)v; } aic_update(d); }
    if (d->is_spmi && off >= 0x720 && off < 0x740) d->sp_en[(off - 0x720) >> 2] = (uint32_t)v;
    if (d->is_spmi && off == 0x778) d->sp_irq &= ~(uint32_t)v;   /* write-1-to-clear: polled mode writes back (status & mask) */
    if (d->is_spmi && off == 0x704 && v) { d->sp_irq |= 1u; a64m_aic_raise(d->m, 234); }   /* 0.4.239 MODELLED (ASSUMPTION): every command "completes" at once with interrupt 234 (0xea): the 2nd entry of the spmi node's "interrupts" list and the only one of them the kernel unmasked (0.4.236 AIC MASK_CLR word 7 = 0x3000470 has bit 10). 204 (0.4.237) is NOT spmi's: it is dart-aop (panic "Max spurious DART/SMMU interrupts", dart-aop) */
    if (d->is_spmi && (off == 0x704 || off == 0xd04) && v && d->sp_n < 16) d->sp_fifo[d->sp_n++] = (uint32_t)v;   /* 0.4.229: non-zero command word -> one response */
    if (d->is_i2c && off == 0x14) d->i2c_st &= ~(uint32_t)v;   /* 0.4.219: status event bits are write-1-to-clear (the driver writes back what it read); bit 24 is a LEVEL, see unk_r */
    if (d->rb && off < sizeof d->rbv) d->rbv[off >> 2] = (uint32_t)v;
    for (int k = 0; k < d->lk_n; k++) if (d->lk_w[k] == off) { d->lk_v[k] = (uint32_t)v & 0x1f; d->lk_set[k] = 1; }
    if (d->ps) { int k = 0; while (k < d->ps_n && d->ps_off[k] != off) k++; if (k < 512) { if (k == d->ps_n) { d->ps_off[k] = off; d->ps_n++; } d->ps_val[k] = (uint32_t)v; } }
    if (d->n < d->cap && d->n < 8) { size_t l = strlen(d->ac); d->n++; if (l < sizeof d->ac - 40) snprintf(d->ac + l, sizeof d->ac - l, " w+0x%llx=0x%llx", (unsigned long long)off, (unsigned long long)v); }
}
static struct Unk *aic_of(A64Machine *m) { for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m && g_unk[k].is_aic) return &g_unk[k]; return NULL; }
const char *a64m_aic_writes(A64Machine *m) { static char b[6000]; size_t l = 0; b[0] = 0; struct Unk *a = aic_of(m); if (!a) return b; for (int k = 0; k < a->aw_cnt && l < sizeof b - 40; k++) l += (size_t)snprintf(b + l, sizeof b - l, " +%x:%ux|%x", a->aw_off[k], a->aw_n[k], a->aw_or[k]); return b; }
const char *a64m_aic_info(A64Machine *m) { static char b[1400]; struct Unk *a = aic_of(m); if (!a) { snprintf(b, sizeof b, "no aic block"); return b; } size_t l = (size_t)snprintf(b, sizeof b, "reads=%u writes=%u last_off=0x%llx first accesses:%s | unmasked irqs (hex):", a->reads, a->writes, (unsigned long long)a->last_off, a->ac); for (unsigned i = 0; i < 512 && l < sizeof b - 8; i++) if (g_aic_clr[i >> 5] >> (i & 31) & 1) l += (size_t)snprintf(b + l, sizeof b - l, " %x", i); return b; }   /* 0.4.435 diagnostic */
int a64m_aic_raise(A64Machine *m, unsigned irq) { struct Unk *a = aic_of(m); if (!a || irq >= 512) return -1; a->aic_pend[irq >> 5] |= 1u << (irq & 31); aic_update(a); return 0; }   /* 0.4.234: a device raises interrupt `irq` */
/* 0.4.53: blocks with a label (the DeviceTree node name) so the log says which one was touched; 0.4.58: up to 360, each with a log budget (`cap` accesses) */
static int add_unknown(A64Machine *m, uint64_t pa, uint64_t size, const char *label, unsigned cap) {
    int slot = -1; for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m && g_unk[k].pa == pa) slot = k;   /* a stale entry of a destroyed machine that got the same address */
    if (slot < 0) for (int k = 0; k < g_unk_n; k++) if (!g_unk[k].m) { slot = k; break; }   /* free slot of a destroyed machine */
    if (slot < 0) { if (g_unk_n >= 360) return -1; slot = g_unk_n++; }
    struct Unk *d = &g_unk[slot], keep = *d; memset(d, 0, sizeof *d); d->m = m; d->pa = pa; d->cap = cap; if (label) snprintf(d->label, sizeof d->label, "'%.38s'", label); d->is_i2c = label && strstr(label, "i2c") != NULL; d->is_dart = label && !strncmp(label, "dart", 4); d->is_spmi = label && !strcmp(label, "spmi"); d->is_aic = label && !strcmp(label, "aic"); if (d->is_aic) for (int z = 0; z < 16; z++) d->aic_mask[z] = 0xffffffffu;
    A64Dev dev = { pa, size, unk_r, unk_w, d, "unknown-soc" };
    if (a64_add_device(&m->cpu, &dev)) { *d = keep; return -1; }   /* 0.4.59: a refused add (overlap with the SAME block added earlier) must not wipe the live entry: this lost the 'aic' +0x4 value in 0.4.58 */
    return 0;
}
/* 0.4.218: direct register access to the unknown block at pa (for tests): write if w, else read into *rv. Returns 0, or -1 when there is no such block. */
int a64m_unknown_rw(A64Machine *m, uint64_t pa, int w, uint64_t off, uint64_t v, uint64_t *rv) { for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m && g_unk[k].pa == pa) { if (w) unk_w(&g_unk[k], off, 4, v); else { uint64_t r = unk_r(&g_unk[k], off, 4); if (rv) *rv = r; } return 0; } return -1; }
int a64m_add_unknown_mmio_named(A64Machine *m, uint64_t pa, uint64_t size, const char *label) { return add_unknown(m, pa, size, label, 40); }
/* 0.4.58: same, for the bulk of DeviceTree nodes: only the first few accesses of each block are logged; the stop report lists every touched block */
int a64m_add_unknown_mmio_auto(A64Machine *m, uint64_t pa, uint64_t size, const char *label) { return add_unknown(m, pa, size, label, 6); }
/* 0.4.58: "name R/W lastoff" of every block of this machine that was touched, for the stop report. Returns the count. */
int a64m_unknown_summary(A64Machine *m, char *out, size_t cap) {
    size_t n = 0; int cnt = 0; if (cap) out[0] = 0;
    for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m && (g_unk[k].reads || g_unk[k].writes)) {
        cnt++; if (n + 260 < cap) n += (size_t)snprintf(out + n, cap - n, " %s=%ur/%uw(last+0x%llx; first:%s)", g_unk[k].label[0] ? g_unk[k].label : "'mem'", g_unk[k].reads, g_unk[k].writes, (unsigned long long)g_unk[k].last_off, g_unk[k].ac);
    }
    return cnt;
}
int a64m_unknown_lastreads(A64Machine *m, char *out, size_t cap) {
    size_t n = 0; int cnt = 0; if (cap) out[0] = 0;
    for (unsigned k = 0; k < 24; k++) { unsigned i = (g_rr_n + k) % 24; if (g_rr[i].m != m || !g_rr[i].label[0]) continue;
        cnt++; if (n + 90 < cap) n += (size_t)snprintf(out + n, cap - n, " %s+0x%llx=0x%llx", g_rr[i].label, (unsigned long long)g_rr[i].off, (unsigned long long)g_rr[i].val); }
    return cnt;
}
int a64m_unknown_set_read(A64Machine *m, uint64_t pa, uint64_t off, uint64_t val) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == pa && g_unk[k].rv_n < 16) { struct Unk *d = &g_unk[k]; d->rv_off[d->rv_n] = off; d->rv_val[d->rv_n++] = val; return 0; }
    return -1;
}
/* 0.4.61: the block's first 4 KiB behave as plain read-back registers (a read returns the last 32-bit value written at that offset, 0 if none) */
int a64m_unknown_set_readback(A64Machine *m, uint64_t pa) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == pa) { g_unk[k].rb = 1; return 0; }
    return -1;
}
/* 0.4.110 MODELLED / ASSUMPTION: a written 32-bit register of this block reads back with bits 7:4 (current state) = bits 3:0 (requested state), as the power-state registers of Apple's PMGR are generally described (target 3:0, actual 7:4); unwritten registers read 0. Needed because ApplePMGR::waitReg32 waits for (reg & 0xf0) == 0xf0 after requesting state 0xf. The real hardware's delay and failure behaviour are NOT modelled. */
/* 0.4.135 MODELLED / ASSUMPTION: after a 32-bit write of v to register `w_off` of the block, the register `r_off` reads (v << 10) | (v << 5) (v masked to 5 bits);
   before any write it reads 0. Decoded from the kernel's _mccEnableCacheMode: it writes the cache mode to mem-controller +0x1c00 and then polls +0x1c04 of
   each of the 4 channels (0x40000 apart) until bits 14:10 and bits 9:5 both equal the mode. Real hardware behaviour (delay, rejection) is NOT modelled. Up to 8 links per block. */
int a64m_unknown_set_modelink(A64Machine *m, uint64_t pa, uint64_t w_off, uint64_t r_off) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == pa) { struct Unk *d = &g_unk[k]; if (d->lk_n >= 8) return -1; d->lk_w[d->lk_n] = w_off; d->lk_r[d->lk_n] = r_off; d->lk_set[d->lk_n] = 0; d->lk_n++; return 0; }
    return -1;
}
/* 0.4.150 MODELLED / ASSUMPTION (PLACEHOLDER values): registers of the AppleT8015TempSensor instances, which lie INSIDE a 'pmgr' block (their DT `reg` overlaps it). The caller (attempt.c) takes every register offset from the node's own
   sensor-offset-* properties; `ts_window` only marks the sensor's range for the access trace, `ts_read` adds one fixed read value (block-relative offset, up to 160 per block). Real sensor behaviour (conversion, alarms, interrupt) is NOT modelled. */
int a64m_unknown_ts_window(A64Machine *m, uint64_t block_pa, uint64_t base_off) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == block_pa) { struct Unk *d = &g_unk[k]; if (d->ts_n >= 16) return -1; d->ts_base[d->ts_n] = base_off; return d->ts_n++; }
    return -1;
}
int a64m_unknown_ts_read(A64Machine *m, uint64_t block_pa, uint64_t off, uint64_t val) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == block_pa) { struct Unk *d = &g_unk[k]; if (d->ts_rn >= 160) return -1; d->ts_off[d->ts_rn] = off; d->ts_val[d->ts_rn++] = val; return 0; }
    return -1;
}
int a64m_unknown_set_psmirror(A64Machine *m, uint64_t pa) {
    for (int k = g_unk_n - 1; k >= 0; k--) if (g_unk[k].m == m && g_unk[k].pa == pa) { g_unk[k].ps = 1; return 0; }
    return -1;
}
int a64m_add_unknown_mmio(A64Machine *m, uint64_t pa, uint64_t size) { return a64m_add_unknown_mmio_named(m, pa, size, NULL); }
static uint64_t fb_r(void *u, uint64_t off, unsigned sz) {
    A64Machine *m = u; (void)sz;
    switch (off) {
    case 0x00: return A64M_FB_W; case 0x04: return A64M_FB_H;
    case 0x08: return m->fb.addr & 0xFFFFFFFFu; case 0x0C: return m->fb.addr >> 32;
    case 0x10: return m->fb.enable; case 0x14: return m->fb.frame; case 0x1C: return 1;
    default: return 0;
    }
}
static void fb_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    A64Machine *m = u; (void)sz;
    switch (off) {
    case 0x08: m->fb.addr = (m->fb.addr & ~0xFFFFFFFFull) | (v & 0xFFFFFFFFu); break;
    case 0x0C: m->fb.addr = (m->fb.addr & 0xFFFFFFFFull) | (v << 32); break;
    case 0x10: m->fb.enable = (uint32_t)(v & 1); break;
    case 0x18: __atomic_add_fetch(&m->fb.frame, 1, __ATOMIC_RELEASE); break;
    default: break;
    }
}
static uint64_t touch_r(void *u, uint64_t off, unsigned sz) {
    A64Machine *m = u; (void)sz;
    switch (off) {
    case 0x00: return m->touch.x; case 0x04: return m->touch.y; case 0x08: return m->touch.state;
    case 0x0C: return m->touch.seq; case 0x10: return m->touch.ctrl; case 0x14: return m->touch.status;
    default: return 0;
    }
}
static void touch_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    A64Machine *m = u; (void)sz;
    if (off == 0x10) { m->touch.ctrl = (uint32_t)(v & 1); update_irq(m); }
    else if (off == 0x14) { if (v & 1) __atomic_and_fetch(&m->touch.status, ~1u, __ATOMIC_ACQ_REL); update_irq(m); }
}
static void sys_w(void *u, uint64_t off, unsigned sz, uint64_t v) {
    A64Machine *m = u; (void)sz;
    if (off == 0) { m->cpu.halted = true; m->cpu.exit_code = (int)(uint32_t)v; }
}
static uint64_t sys_r(void *u, uint64_t off, unsigned sz) { (void)u; (void)off; (void)sz; return 0; }

void a64m_touch(A64Machine *m, int x, int y, bool down) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    m->touch.x = (uint32_t)x; m->touch.y = (uint32_t)y; m->touch.state = down;
    __atomic_add_fetch(&m->touch.seq, 1, __ATOMIC_RELEASE);
    __atomic_or_fetch(&m->touch.status, 1u, __ATOMIC_ACQ_REL);
    update_irq(m);
}

void a64m_set_video(A64Machine *m, uint64_t pa, uint32_t w, uint32_t h, uint32_t rowbytes) { m->vid.pa = pa; m->vid.w = w; m->vid.h = h; m->vid.rowbytes = rowbytes; m->vid.sum = 0; m->vid.frame = 0; }
int64_t a64m_frame(A64Machine *m, uint32_t *dst, int w, int h) {
    if (!m->fb.enable) {
        /* IPSW attempt: the linear framebuffer described by boot_args.Video. There is no "present" signal, so the frame counter advances when the content changes. */
        if (!m->vid.pa || w != (int)m->vid.w || h != (int)m->vid.h) return -1;
        uint8_t *p = a64m_ram_at(m, m->vid.pa, (uint64_t)m->vid.rowbytes * m->vid.h);
        if (!p) return -1;
        uint64_t sum = 1469598103934665603ull;
        for (int y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *)(const void *)(p + (size_t)y * m->vid.rowbytes);
            for (int x = 0; x < w; x++) { dst[(size_t)y * (size_t)w + (size_t)x] = 0xFF000000u | (s[x] & 0xFFFFFFu); sum = (sum ^ s[x]) * 1099511628211ull; }
        }
        if (sum != m->vid.sum) { m->vid.sum = sum; m->vid.frame++; }
        return m->vid.frame;
    }
    uint64_t bytes = (uint64_t)A64M_FB_W * A64M_FB_H * 4;
    uint8_t *p = a64m_ram_at(m, m->fb.addr, bytes);
    if (!p || w != A64M_FB_W || h != A64M_FB_H) return -1;
    const uint32_t *s = (const uint32_t *)(const void *)p;
    for (int k = 0; k < A64M_FB_W * A64M_FB_H; k++) dst[k] = 0xFF000000u | (s[k] & 0xFFFFFFu);
    return (int64_t)__atomic_load_n(&m->fb.frame, __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------- lifecycle */
static void user_out(void *u, const char *b, size_t n) { log_raw((A64Machine *)u, b, n); }
static void unimpl_svc(void *u, bool bsd, uint64_t num) {
    a64m_log((A64Machine *)u, "[emu] unimplemented %s syscall %llu (returned ENOSYS)\n", bsd ? "BSD/XNU" : "Linux", (unsigned long long)num);
}

static A64Machine *alloc_m(A64MachineKind k, uint64_t base, uint64_t size) {
    size = (size + 0xFFFF) & ~0xFFFFull;
    A64Machine *m = calloc(1, sizeof *m); if (!m) return NULL;
    m->kind = k; m->ram_base = base; m->ram_size = size;
    m->ram = calloc(1, size);
    for (int s = 0; s < 3; s++) m->slog[s] = calloc(1, LOG_CAP);
    if (!m->ram || !m->slog[0] || !m->slog[1] || !m->slog[2]) { free(m->ram); for (int s = 0; s < 3; s++) free(m->slog[s]); free(m); return NULL; }
    pthread_mutex_init(&m->lock, NULL);
    a64_init(&m->cpu, m->ram, base, size);
    m->state = A64M_CREATED;
    return m;
}

A64Machine *a64m_create_system(uint64_t ram_size) { return a64m_create_system_at(ram_size, A64M_RAM_BASE); }
A64Machine *a64m_create_system_at(uint64_t ram_size, uint64_t base) {
    g_diag_dumped = 0;
    A64Machine *m = alloc_m(A64M_KIND_SYSTEM, base, ram_size);
    if (!m) return NULL;
    a64_system_init(&m->cpu);
    A64Dev d[4] = {
        { A64M_UART_BASE, 0x1000, uart_r, uart_w, m, "uart" },
        { A64M_FB_BASE, 0x1000, fb_r, fb_w, m, "fb" },
        { A64M_TOUCH_BASE, 0x1000, touch_r, touch_w, m, "touch" },
        { A64M_SYSCON, 0x1000, sys_r, sys_w, m, "syscon" },
    };
    for (int k = 0; k < 4; k++) a64_add_device(&m->cpu, &d[k]);
    m->cpu.pc = m->entry = base;
    return m;
}
A64Machine *a64m_create_user(uint64_t base, uint64_t size) {
    A64Machine *m = alloc_m(A64M_KIND_USER, base, size);
    if (!m) return NULL;
    m->cpu.out = user_out; m->cpu.ud = m; m->cpu.on_unimpl_svc = unimpl_svc;
    return m;
}
void a64m_destroy(A64Machine *m) {
    if (!m) return;
    for (int k = 0; k < g_unk_n; k++) if (g_unk[k].m == m) memset(&g_unk[k], 0, sizeof g_unk[k]);   /* 0.4.110: drop the unknown-block entries of this machine (a later machine can get the same address and would inherit them) */
    for (unsigned k = 0; k < 24; k++) if (g_rr[k].m == m) g_rr[k].label[0] = 0;
    pthread_mutex_destroy(&m->lock); free(m->cpu.cov); free(m->cpu.callseen); free(m->ram); for (int s = 0; s < 3; s++) free(m->slog[s]); free(m);
}
A64 *a64m_cpu(A64Machine *m) { return &m->cpu; }
void a64m_mark_raw(A64Machine *m) { m->raw = true; }
static uint64_t impl_reg(void *u, unsigned k, bool wr, uint64_t v);
void a64m_mark_attempt(A64Machine *m, uint64_t vb, uint64_t pb) { m->cpu.stub_impl_regs = true; m->cpu.on_impl_reg = impl_reg; m->cpu.impl_ud = m; m->attempt = true; m->collapse = true; m->virt_base = vb; m->phys_base = pb; }
void a64m_set_pc(A64Machine *m, uint64_t pc) { m->cpu.pc = m->entry = pc; }
void a64m_abort(A64Machine *m, const char *why) {
    a64m_log(m, "\n[boot] ATTEMPT ENDED BEFORE ANY GUEST INSTRUCTION RAN: %s\n", why);
    m->state = A64M_FAULT; m->reported_end = true;
}
void a64m_set_mpidr(A64Machine *m, uint64_t v) { m->cpu.mpidr = v; }
void a64m_set_store_watch(A64Machine *m, uint64_t pa, uint64_t len) { m->cpu.sw_lo = pa; m->cpu.sw_hi = pa + len; }
static uint64_t g_tbl_va, g_tbl_stride, g_tbl_n;
static uint64_t g_dt_phys, g_diag_va[6], g_diag_len[6], g_ddv[3], g_ddl[3]; static unsigned g_diag_n;
void a64m_set_diag_code(A64Machine *m, uint64_t va, uint64_t len) { (void)m; if (!va) { g_diag_n = 0; return; } if (g_diag_n < 6) { g_diag_va[g_diag_n] = va; g_diag_len[g_diag_n] = len; g_diag_n++; } }
void a64m_set_pc_probe2(A64Machine *m, uint64_t pc, unsigned reg, uint64_t val) { m->cpu.pp2_pc = pc; m->cpu.pp2_reg = reg; m->cpu.pp2_val = val; m->cpu.pp2_hit = 0; }
void a64m_set_pc_probe(A64Machine *m, uint64_t pc, unsigned reg, uint64_t val) { m->cpu.pp_pc = pc; m->cpu.pp_reg = reg; m->cpu.pp_val = val; m->cpu.pp_hit = 0; }
static int g_vw = A64M_VID_W, g_vh = A64M_VID_H;
static int vclamp(int v, int d) { if (v < 128 || v > 2400) return d; return v & ~1; }
void a64m_set_video_size(int w, int h) { g_vw = vclamp(w, A64M_VID_W); g_vh = vclamp(h, A64M_VID_H); }
int a64m_video_w(void) { return g_vw; }
int a64m_video_h(void) { return g_vh; }
void a64m_nm_add_dt_name(A64Machine *m, const char *name) { a64_nm_add_dt_name(&m->cpu, name); }
void a64m_set_diag_table(A64Machine *m, uint64_t va, uint64_t stride, uint64_t n) { (void)m; g_tbl_va = va; g_tbl_stride = stride; g_tbl_n = n; }
void a64m_set_diag_data(A64Machine *m, uint64_t va, uint64_t len) { a64m_set_diag_data2(m, 0, va, len); }
void a64m_set_diag_data2(A64Machine *m, unsigned idx, uint64_t va, uint64_t len) { (void)m; if (idx < 3) { g_ddv[idx] = va; g_ddl[idx] = len; } }
void a64m_set_quiet_exc(A64Machine *m, int q) { (void)m; g_quiet_exc = q; }
void a64m_set_value_watch(A64Machine *m, uint64_t v) { m->cpu.vw_val = v; }
void a64m_set_store_watch2(A64Machine *m, int i, uint64_t pa, uint64_t len) { m->cpu.swb[i].lo = pa; m->cpu.swb[i].hi = pa + len; }
static uint64_t g_ref_va[4]; static unsigned g_ref_n;
void a64m_set_coverage(A64Machine *m, uint64_t base, uint64_t len) {
    free(m->cpu.cov); free(m->cpu.callseen); m->cpu.cov = calloc((size_t)(len / 32 + 1), 1); m->cpu.callseen = calloc((size_t)(len / 32 + 1), 1);
    m->cpu.cov_base = base; m->cpu.cov_len = len; m->cpu.call_total = 0;
    m->cpu.e6_pc = 0xfffffff007bc5890ull;   /* 0.4.369: buf_seterror-like setter */
    m->cpu.cap_pc = 0xfffffff007a43ddcull;   /* 0.4.357: Assert(file,line,expr) reached from load_init_program (lr 7ed1ef8) */
    m->cpu.wpc[0] = 0xfffffff008151d70ull; m->cpu.wpc[1] = 0xfffffff008151d24ull;   /* 0.4.352: the two indirect calls of the SecureRoot-like platform-function handler (branch that answers the bootstrap thread; x1 = symbol name) */ /* old: 0xfffffff008149510ull   /* 0.4.310 diagnostic: the vtable+0x5c0 call in iovmMapMemory; x0 = the DART mapper object */
    if (!m->cpu.cov || !m->cpu.callseen) { free(m->cpu.cov); free(m->cpu.callseen); m->cpu.cov = m->cpu.callseen = NULL; m->cpu.cov_len = 0; }
}
void a64m_set_ref_scan(A64Machine *m, unsigned idx, uint64_t va) { (void)m; if (idx < 4) { g_ref_va[idx] = va; if (idx + 1 > g_ref_n) g_ref_n = idx + 1; } }
/* find instructions that reference a kernel global: ADRP rd, page ; then within 6 words LDR/STR (unsigned offset) or ADD with Rn==rd and the exact page offset */
static void dump_refs(A64Machine *m) {
    if (!m->cpu.cov) return;
    { unsigned tot = m->cpu.call_total, n = tot < 40 ? tot : 40;   /* 0.4.20: 160 lines cut the log (20000 chars) before the watches */
      a64m_log(m, "[stop] the last %u of %u functions called for the FIRST time (bl targets), oldest first, 8 per line:\n", n, tot);
      for (unsigned q = 0; q < n; q += 8) { char l2[200]; int o2 = 0; for (unsigned r = q; r < q + 8 && r < n; r++) o2 += snprintf(l2 + o2, sizeof l2 - (size_t)o2, " %llx", (unsigned long long)m->cpu.callring[(tot - n + r) % 160]); a64m_log(m, "[stop] %s\n", l2); } }
    if (m->cpu.nm_on) {   /* 0.4.79 */
        a64m_log(m, "[stop] DeviceTree names probe (HEURISTIC): %u lowercase dashed strings passed as x1/x2 to calls look like DT property names but are NOT in the DeviceTree (first %u, in order of first use; pc = call site; some may be other registry keys, not DT properties):\n", m->cpu.nm_n, m->cpu.nm_n);
        char l3[220]; int o3 = 0;
        for (unsigned q = 0; q < m->cpu.nm_n && q < 48; q++) {   /* 0.4.81: at most 48 names, the log is cut at 20000 characters and the probe list swallowed the exception list in 0.4.80 */
            o3 += snprintf(l3 + o3, sizeof l3 - (size_t)o3, " %s@%llx", m->cpu.nm[q], (unsigned long long)m->cpu.nm_pc[q]);
            if (o3 > 150 || q + 1 == m->cpu.nm_n || q + 1 == 48) { a64m_log(m, "[stop]%s\n", l3); o3 = 0; l3[0] = 0; }
        }
    }
    if (!g_ref_n) return;
    uint64_t lo = m->cpu.cov_base, len = m->cpu.cov_len;
    for (unsigned t = 0; t < g_ref_n; t++) {
        uint64_t tv = g_ref_va[t]; if (!tv) continue;
        a64m_log(m, "[stop] instructions that reference kernel global 0x%llx (ADRP+LDR/STR/ADD; '+' ran, '-' never ran; l = load, s = STORE, a = address taken):\n", (unsigned long long)tv);
        char ln[240]; int o = 0; unsigned found = 0, quiet = 0;
        uint8_t *base = a64m_ram_at(m, m->phys_base + (lo - m->virt_base), (size_t)len);
        if (!base) { a64m_log(m, "[stop]  (image not addressable)\n"); continue; }
        for (uint64_t off = 0; off + 4 <= len && found < 400; off += 4) {
            uint32_t w; memcpy(&w, base + off, 4);
            if ((w & 0x9f000000u) != 0x90000000u) continue;
            int64_t imm = (int64_t)(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3u)); if (imm & 0x100000) imm -= 0x200000;
            uint64_t page = ((lo + off) & ~0xfffull) + (uint64_t)(imm << 12);
            if (page != (tv & ~0xfffull)) continue;
            unsigned rd = w & 31;
            for (int k = 1; k <= 6 && off + 4 + 4 * (uint64_t)k <= len + 4; k++) {
                if (off + 4 * (uint64_t)k + 4 > len) break;
                uint32_t v; memcpy(&v, base + off + 4 * (uint64_t)k, 4);
                if (((v >> 5) & 31) != rd) continue;
                uint64_t offv = (uint64_t)-1;
                if ((v & 0x3b000000u) == 0x39000000u) offv = (uint64_t)((v >> 10) & 0xfff) << (v >> 30);
                else if ((v & 0xff800000u) == 0x91000000u) offv = (v >> 10) & 0xfff;
                if (offv != (tv & 0xfff)) continue;
                uint64_t pcv = lo + off + 4 * (uint64_t)k; uint64_t co = pcv - lo;
                int ran = (m->cpu.cov[co >> 5] >> ((co >> 2) & 7)) & 1;
                char kind = ((v & 0xff800000u) == 0x91000000u) ? 'a' : (((v >> 22) & 3) == 0 ? 's' : 'l');   /* a = address (ADD), s = store, l = load */
                found++;
                if (!ran && kind == 'l') { quiet++; break; }   /* never-executed plain loads are only counted */
                o += snprintf(ln + o, sizeof ln - (size_t)o, " %c%c%llx", ran ? '+' : '-', kind, (unsigned long long)pcv);
                if (o > 200) { a64m_log(m, "[stop] %s\n", ln); o = 0; }
                break;
            }
        }
        if (o) a64m_log(m, "[stop] %s\n", ln);
        a64m_log(m, "[stop]  %u references in all, %u of them never-executed plain loads (not listed)\n", found, quiet);
        if (!found) a64m_log(m, "[stop]  (none found)\n");
    }
}
/* 0.4.72: find a text in the kernel image, find the code that references it (ADRP+ADD) and dump the code around the first reference. Valid for any kernelcache; nothing about the text is assumed. */
static void dump_code_cap(A64Machine *m, uint64_t lo, uint64_t hi, uint64_t cap);
static char g_dstr[2][96]; static uint64_t g_dstr_before[2], g_dstr_after[2];
void a64m_set_diag_string(A64Machine *m, unsigned idx, const char *text, uint64_t before, uint64_t after) {
    (void)m; if (idx >= 2 || !text) return;
    snprintf(g_dstr[idx], sizeof g_dstr[idx], "%s", text); g_dstr_before[idx] = before; g_dstr_after[idx] = after;
}
static void dump_strrefs(A64Machine *m) {
    if (!m->cpu.cov) return;
    uint64_t lo = m->cpu.cov_base, len = m->cpu.cov_len;
    uint8_t *base = a64m_ram_at(m, m->phys_base + (lo - m->virt_base), (size_t)len); if (!base) return;
    for (unsigned t = 0; t < 2; t++) {
        if (!g_dstr[t][0]) continue;
        size_t tl = strlen(g_dstr[t]); uint64_t sv = 0;
        for (uint64_t off = 0; off + tl <= len; off++) if (base[off] == (uint8_t)g_dstr[t][0] && !memcmp(base + off, g_dstr[t], tl)) { uint64_t st = off; while (st > 0 && base[st - 1]) st--; sv = lo + st; break; }
        if (!sv) { a64m_log(m, "[stop] text \"%s\" not found in the kernel image\n", g_dstr[t]); continue; }
        uint64_t ref = 0;
        for (uint64_t off = 0; off + 8 <= len && !ref; off += 4) {
            uint32_t w; memcpy(&w, base + off, 4); if ((w & 0x9f000000u) != 0x90000000u) continue;
            int64_t imm = (int64_t)(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3u)); if (imm & 0x100000) imm -= 0x200000;
            if ((((lo + off) & ~0xfffull) + (uint64_t)(imm << 12)) != (sv & ~0xfffull)) continue;
            for (int k = 1; k <= 6 && off + 4 * (uint64_t)k + 4 <= len; k++) { uint32_t v; memcpy(&v, base + off + 4 * (uint64_t)k, 4);
                if (((v >> 5) & 31) == (w & 31) && (v & 0xff800000u) == 0x91000000u && ((v >> 10) & 0xfff) == (sv & 0xfff)) { ref = lo + off; break; } }
        }
        a64m_log(m, "[stop] text \"%s\" is at kernel virtual 0x%llx; first code that references it: %s0x%llx (dump: %llu bytes before, %llu after)\n", g_dstr[t], (unsigned long long)sv, ref ? "" : "none found ", (unsigned long long)ref, (unsigned long long)g_dstr_before[t], (unsigned long long)g_dstr_after[t]);
        if (ref) dump_code_cap(m, ref - g_dstr_before[t], ref + g_dstr_after[t], g_dstr_before[t] + g_dstr_after[t] + 0x20);
    }
}
void a64m_set_pc_trace(A64Machine *m, uint64_t va) { m->cpu.tr_pc = va; m->cpu.tr_n = 0; m->cpu.tr_on = 0; }
void a64m_set_call_trace(A64Machine *m, uint64_t va) { m->cpu.ctr_pc = va; m->cpu.ctr_mode = 0; }
void a64m_set_call_trace_regs(A64Machine *m, uint64_t va) { m->cpu.ctr_pc = va; m->cpu.ctr_mode = 1; }
void a64m_set_dt_phys(A64Machine *m, uint64_t pa) { (void)m; g_dt_phys = pa; }
static A64SocNode g_soc[300]; static int g_soc_n;
void a64m_set_soc_nodes(A64Machine *m, const A64SocNode *nodes, int n) { (void)m; if (n > 300) n = 300; if (n < 0) n = 0; memcpy(g_soc, nodes, (size_t)n * sizeof *nodes); g_soc_n = n; }
void a64m_set_boot_arg(A64Machine *m, uint64_t v) { m->cpu.x[0] = v; m->cpu.watch_lo = v; m->cpu.watch_hi = v + 0x400; }
uint8_t *a64m_ram_at(A64Machine *m, uint64_t a, uint64_t len) {
    if (a < m->ram_base) return NULL;
    uint64_t o = a - m->ram_base;
    if (o > m->ram_size || len > m->ram_size - o) return NULL;
    return m->ram + o;
}
int a64m_load_raw(A64Machine *m, const uint8_t *data, size_t len, uint64_t addr) {
    if (!addr) addr = m->ram_base;
    uint8_t *p = a64m_ram_at(m, addr, len);
    if (!p) return -1;
    memcpy(p, data, len);
    m->cpu.pc = m->entry = addr;
    return 0;
}

/* Apple implementation-defined registers (IPSW attempts only).  Model: a register reads back the last value written
 * (0 if never written); a few registers have documented-by-observation "ready" bits forced on so that the kernel's
 * wait loops can proceed.  This is an ASSUMPTION made to see how far the kernel gets, not real hardware behaviour.
 * Each distinct (register, direction) is logged once. */
#define IMPL_MAX 96
static const struct { unsigned key; uint64_t force_bits; const char *why; } IMPL_READY[] = {
    { (3u << 14) | (4u << 11) | (15u << 7) | (0u << 3) | 4u, 0x2, "kernel polls bit 1 right before enabling the MMU; assumed 'ready'" },
};
static uint64_t impl_reg(void *u, unsigned k, bool wr, uint64_t v) {
    A64Machine *m = u; int idx = -1;
    for (unsigned i = 0; i < m->impl_n; i++) if (m->impl_key[i] == k) { idx = (int)i; break; }
    if (idx < 0 && m->impl_n < IMPL_MAX) { idx = (int)m->impl_n++; m->impl_key[idx] = k; m->impl_val[idx] = 0; m->impl_seen[idx] = 0; }
    unsigned bit = wr ? 2u : 1u;
    if (idx >= 0 && !(m->impl_seen[idx] & bit)) {
        m->impl_seen[idx] |= bit;
    }
    if (wr) { if (idx >= 0) m->impl_val[idx] = v; return 0; }
    uint64_t r = idx >= 0 ? m->impl_val[idx] : 0;
    for (unsigned i = 0; i < sizeof IMPL_READY / sizeof *IMPL_READY; i++)
        if (IMPL_READY[i].key == k) {
            m->impl_seen[idx] |= 4u;
            r |= IMPL_READY[i].force_bits;
        }
    return r;
}

/* --------------------------------------------------------------- running */
void a64m_request_stop(A64Machine *m) { m->stop = 1; }
A64MState a64m_state(const A64Machine *m) { return m->state; }
int a64m_exit_code(const A64Machine *m) { return m->cpu.exit_code; }

#define STUCK_CHUNKS 2500        /* 2500 x 20000 = 50M instructions without leaving a 64 KiB window */
#define STUCK_WINDOW 0x10000
static void dump_code(A64Machine *m, uint64_t lo, uint64_t hi);
static void dump_code_cap(A64Machine *m, uint64_t lo, uint64_t hi, uint64_t cap);
static bool read_word(A64Machine *m, uint64_t a, uint32_t *x);
static bool peek64_dbg(A64Machine *m, uint64_t a, uint64_t *o) { uint32_t lo, hi; if (!read_word(m, a, &lo) || !read_word(m, a + 4, &hi)) return false; *o = lo | ((uint64_t)hi << 32); return true; }
static int text_at(A64Machine *m, uint64_t va, char *out, size_t cap) {   /* printable NUL-terminated text at va (up to cap-1 chars), else -1 */
    size_t len = 0;
    for (uint64_t q = 0; q < 252 && len + 5 < cap; q += 4) {
        uint32_t w; if (!read_word(m, va + q, &w)) return -1;
        for (int z = 0; z < 4; z++) { unsigned ch = (w >> (8 * z)) & 0xFF; if (!ch) { out[len] = 0; return (int)len; } if (ch < 32 || ch > 126) return -1; out[len++] = (char)ch; }
    }
    out[len] = 0; return (int)len;
}
static const char *ec_name(uint32_t ec) {
    switch (ec) {
    case 0x00: return "unknown/undefined instruction"; case 0x01: return "WFI/WFE trap"; case 0x15: return "SVC"; case 0x18: return "MSR/MRS/SYS trap";
    case 0x20: return "instruction abort (lower EL)"; case 0x21: return "instruction abort (same EL)"; case 0x22: return "PC alignment";
    case 0x24: return "data abort (lower EL)"; case 0x25: return "data abort (same EL)"; case 0x26: return "SP alignment"; case 0x3c: return "BRK"; default: return "other";
    }
}
static void dump_exceptions(A64Machine *m) {
    const A64 *c = &m->cpu;
    if (c->mode != A64_MODE_SYSTEM || !c->exc_first_n) return;
    a64m_log(m, "[stop] first %d exceptions the guest took (instruction count, kind, cause, ELR, FAR):\n", c->exc_first_n);
    for (int k = 0; k < c->exc_first_n; k++) {
        uint32_t esr = c->exc_first[k].esr;
        if (c->exc_first[k].kind == 0)
            a64m_log(m, "[stop]  #%d at instr %llu: sync ESR=0x%08x (%s, ISS 0x%x) ELR=0x%llx FAR=0x%llx\n", k, (unsigned long long)c->exc_first[k].steps, esr, ec_name(esr >> 26), esr & 0x1FFFFFF,
                     (unsigned long long)c->exc_first[k].elr, (unsigned long long)c->exc_first[k].far);
        else a64m_log(m, "[stop]  #%d at instr %llu: interrupt ELR=0x%llx\n", k, (unsigned long long)c->exc_first[k].steps, (unsigned long long)c->exc_first[k].elr);
    }
    if (c->exc0_valid) {
        a64m_log(m, "[stop] registers at the captured exception (#%d: the first one that is not a kernel-VA translation fault) (ELR=0x%llx):\n", c->exc0_k, (unsigned long long)c->exc_first[c->exc0_k].elr);
        for (int i = 0; i < 31; i += 4) {
            char b[200]; int n = 0;
            for (int k = i; k < i + 4 && k < 31; k++) n += snprintf(b + n, sizeof b - (size_t)n, "x%-2d=%016llx ", k, (unsigned long long)c->exc0_x[k]);
            a64m_log(m, "[stop]  %s\n", b);
        }
        a64m_log(m, "[stop]  sp=%016llx\n", (unsigned long long)c->exc0_sp);
        dump_code(m, c->exc_first[c->exc0_k].elr, c->exc_first[c->exc0_k].elr);
        a64m_log(m, "[stop] call chain at the first exception (frame pointer: return address):");
        { char cb[400]; int cn = 0; for (unsigned q = 0; q < c->exc0_chain_n; q++) cn += snprintf(cb + cn, sizeof cb - (size_t)cn, " %llx:%llx", (unsigned long long)c->exc0_chain[q][0], (unsigned long long)c->exc0_chain[q][1]); a64m_log(m, "%s\n", cb); }
        for (int r = 0; r < 8; r++) {
            char sb[260]; int len = text_at(m, c->exc0_x[r], sb, sizeof sb);
            if (len >= 4) { a64m_log(m, "[stop]  x%d points to the text \"%s\"\n", r, sb); }
            else if (c->exc0_x[r] >= 0xffffffe000000000ull && !(c->exc0_x[r] & 7)) {
                const uint64_t *q8 = c->exc0_q[r]; int okw = c->exc0_qok[r];
                if (okw) {
                    a64m_log(m, "[stop]  x%d points to the 64-bit words %llx %llx %llx %llx %llx %llx %llx %llx\n", r, (unsigned long long)q8[0], (unsigned long long)q8[1], (unsigned long long)q8[2], (unsigned long long)q8[3], (unsigned long long)q8[4], (unsigned long long)q8[5], (unsigned long long)q8[6], (unsigned long long)q8[7]);
                    a64m_log(m, "[stop]  x%d, words 8..15: %llx %llx %llx %llx %llx %llx %llx %llx\n", r, (unsigned long long)q8[8], (unsigned long long)q8[9], (unsigned long long)q8[10], (unsigned long long)q8[11], (unsigned long long)q8[12], (unsigned long long)q8[13], (unsigned long long)q8[14], (unsigned long long)q8[15]);
                    for (int q = 0; q < 16; q++) { char tb[260]; if (q8[q] >= 0xffffffe000000000ull && text_at(m, q8[q], tb, sizeof tb) >= 4) a64m_log(m, "[stop]    x%d[%d] points to the text \"%s\"\n", r, q, tb); }
                }
            }
        }
        if ((c->exc_first[c->exc0_k].esr >> 26) != 0 && !g_quiet_exc) {   /* an undefined-instruction first exception is the guest's own panic trap: these lists would only be noise */
        a64m_log(m, "[stop] the last %u data accesses before the first exception (pc: R/W size virtual-address = value):\n", c->exc0_acc_n);
        for (unsigned q = 0; q < c->exc0_acc_n; q += 2) {
            char ab[300]; int an = 0;
            for (unsigned k = q; k < q + 2 && k < c->exc0_acc_n; k++) an += snprintf(ab + an, sizeof ab - (size_t)an, " %llx:%s%u %llx=%llx", (unsigned long long)c->exc0_acc[k].pc, c->exc0_acc[k].wr ? "W" : "R", c->exc0_acc[k].sz, (unsigned long long)c->exc0_acc[k].va, (unsigned long long)c->exc0_acc[k].val);
            a64m_log(m, "[stop] %s\n", ab);
        }
        a64m_log(m, "[stop] the %u instructions executed just before the first exception (pc:word):\n", c->exc0_pcn);
        for (unsigned q = 0; q < c->exc0_pcn; q += 4) {
            char b[300]; int n = 0;
            for (unsigned k = q; k < q + 4 && k < c->exc0_pcn; k++) { uint32_t x = 0; bool ok = read_word(m, c->exc0_pc[k], &x); n += snprintf(b + n, sizeof b - (size_t)n, " %llx:%s", (unsigned long long)c->exc0_pc[k], "") ; if (ok) n += snprintf(b + n, sizeof b - (size_t)n, "%08x", x); else n += snprintf(b + n, sizeof b - (size_t)n, "????????"); }
            a64m_log(m, "[stop] %s\n", b);
        }
        }
        char w[400];
        if (c->exc_first[c->exc0_k].esr >> 26 == 0x25 || c->exc_first[c->exc0_k].esr >> 26 == 0x24) { a64_walk_trace((A64 *)c, c->exc_first[c->exc0_k].far, w, sizeof w); a64m_log(m, "[stop] first fault address: %s\n", w); }
    }
}
/* text strings referenced by ADRP + ADD pairs in a code window (they name the function: panic texts, log messages) */
static void dump_bytes_va(A64Machine *m, const char *tag, uint64_t va, unsigned n) {   /* 0.4.51 diagnostic: hex words plus printable text of kernel virtual memory */
    char hx[700], tx[200]; int a = 0, b = 0;
    for (unsigned k = 0; k < n; k += 4) { uint32_t w; if (!read_word(m, va + k, &w)) { a += snprintf(hx + a, sizeof hx - (size_t)a, " ????????"); for (int z = 0; z < 4; z++) tx[b++] = '?'; continue; }
        a += snprintf(hx + a, sizeof hx - (size_t)a, " %08x", w); for (int z = 0; z < 4; z++) { unsigned ch = (w >> (8 * z)) & 0xFF; tx[b++] = (ch >= 32 && ch < 127) ? (char)ch : '.'; } }
    tx[b] = 0; a64m_log(m, "[stop] bytes %s @0x%llx:%s |%s|\n", tag, (unsigned long long)va, hx, tx);
}
static void dump_strs(A64Machine *m, uint64_t va, uint64_t len) {
    uint8_t *p = a64m_ram_at(m, m->phys_base + (va - m->virt_base), len); if (!p) return;
    for (uint64_t o = 0; o + 16 <= len; o += 4) {
        uint32_t w; memcpy(&w, p + o, 4);
        if ((w & 0x9f000000u) != 0x90000000u) continue;
        int64_t imm = (int64_t)(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3u)); if (imm & 0x100000) imm -= 0x200000;
        uint64_t page = ((va + o) & ~0xfffull) + (uint64_t)(imm << 12); unsigned rd = w & 31;
        for (int k = 1; k <= 3; k++) {
            uint32_t v; memcpy(&v, p + o + 4 * (uint64_t)k, 4);
            if ((v & 0xff800000u) != 0x91000000u || ((v >> 5) & 31) != rd) continue;
            uint64_t a = page + ((v >> 10) & 0xfff);
            uint8_t *s = a64m_ram_at(m, m->phys_base + (a - m->virt_base), 64); if (!s) break;
            char t[72]; int n = 0; while (n < 64 && s[n] >= 32 && s[n] < 127) { t[n] = (char)s[n]; n++; } t[n] = 0;
            if (n >= 4 && s[n] == 0) a64m_log(m, "[stop]  string at 0x%llx referenced at 0x%llx: \"%s\"\n", (unsigned long long)a, (unsigned long long)(va + o), t);
            break;
        }
    }
}
static uint64_t g_caller_va;
void a64m_set_caller_scan(A64Machine *m, uint64_t va) { (void)m; g_caller_va = va; }
/* the function containing g_caller_va (nearest PACIBSP, hint #27, going backwards) and everything that calls it with BL */
static void dump_callers(A64Machine *m) {
    if (!g_caller_va || !m->cpu.cov) return;
    uint64_t lo = m->cpu.cov_base, len = m->cpu.cov_len;
    uint8_t *base = a64m_ram_at(m, m->phys_base + (lo - m->virt_base), (size_t)len); if (!base) return;
    uint64_t f = g_caller_va & ~3ull, lim = f > lo + 0x1000 ? f - 0x1000 : lo; uint32_t w = 0;
    for (; f >= lim; f -= 4) { memcpy(&w, base + (f - lo), 4); if (w == 0xd503237fu) break; }
    if (w != 0xd503237fu) { a64m_log(m, "[stop] no function start (PACIBSP) found within 0x1000 bytes before 0x%llx\n", (unsigned long long)g_caller_va); return; }
    a64m_log(m, "[stop] the function containing 0x%llx starts at 0x%llx; BL calls to it (+ ran / - never ran), with the start of the calling function:\n", (unsigned long long)g_caller_va, (unsigned long long)f);
    unsigned n = 0;
    for (uint64_t off = 0; off + 4 <= len && n < 16; off += 4) {
        uint32_t v; memcpy(&v, base + off, 4);
        if ((v & 0xfc000000u) != 0x94000000u) continue;
        int64_t d = (int64_t)(v & 0x3ffffffu); if (d & 0x2000000) d -= 0x4000000;
        if (lo + off + (uint64_t)(d * 4) != f) continue;
        uint64_t cf = lo + off; uint32_t z = 0; uint64_t cl = cf > lo + 0x4000 ? cf - 0x4000 : lo;
        for (; cf >= cl; cf -= 4) { memcpy(&z, base + (cf - lo), 4); if (z == 0xd503237fu) break; }
        uint64_t co = lo + off - lo; int ran = (m->cpu.cov[co >> 5] >> ((co >> 2) & 7)) & 1;
        a64m_log(m, "[stop]  %cbl at 0x%llx, in the function starting at 0x%llx\n", ran ? '+' : '-', (unsigned long long)(lo + off), (unsigned long long)(z == 0xd503237fu ? cf : 0));
        n++;
    }
    if (!n) a64m_log(m, "[stop]  (no direct BL to it found; it may be called through a pointer)\n");
}
void a64m_set_block_hist(A64Machine *m, uint64_t pa, uint64_t len) { m->cpu.bh_lo = pa; m->cpu.bh_hi = pa + len; m->cpu.bh_n = 0; g_diag_dumped = 0; }
void a64m_set_arg_watch(A64Machine *m, uint64_t v) { m->cpu.aw_val = v; m->cpu.aw_n = 0; }
unsigned a64m_arg_watch_count(A64Machine *m) { return m->cpu.aw_n; }
unsigned a64m_block_hist_count(A64Machine *m) { return m->cpu.bh_n; }
static void dump_diag(A64Machine *m) {
    if (g_diag_dumped) return;           /* the stuck-loop path and the final report both call this: print once */
    g_diag_dumped = 1;
    if (m->cpu.aw_val) {
        a64m_log(m, "[stop] calls with x0 or x1 == 0x%llx (C: step pc target x1 x2) and returns with x0 == it (R: step pc return-to), %u shown:\n", (unsigned long long)m->cpu.aw_val, m->cpu.aw_n);
        for (unsigned k = 0; k < m->cpu.aw_n; k++) a64m_log(m, "[stop] %c %llu %llx %llx %llx %llx\n", m->cpu.aw[k].kind, (unsigned long long)m->cpu.aw[k].steps, (unsigned long long)m->cpu.aw[k].pc, (unsigned long long)m->cpu.aw[k].tgt, (unsigned long long)m->cpu.aw[k].x1, (unsigned long long)m->cpu.aw[k].x2);
    }
    if (m->cpu.bh_hi) {
        a64m_log(m, "[stop] history of accesses to physical block 0x%llx+0x%llx (stores, plus loads of the first word; %u shown; step pc lr caller-of-lr off size val):\n", (unsigned long long)m->cpu.bh_lo, (unsigned long long)(m->cpu.bh_hi - m->cpu.bh_lo), m->cpu.bh_n);
        for (unsigned k = 0; k < m->cpu.bh_n; k++) a64m_log(m, "[stop] %c %llu %llx %llx %llx +%llx %u %llx\n", m->cpu.bh[k].wr ? 'W' : 'R', (unsigned long long)m->cpu.bh[k].steps, (unsigned long long)m->cpu.bh[k].pc, (unsigned long long)m->cpu.bh[k].lr, (unsigned long long)m->cpu.bh[k].lr2, (unsigned long long)m->cpu.bh[k].off, m->cpu.bh[k].sz, (unsigned long long)m->cpu.bh[k].val);
    }
    if (m->cpu.pp_pc) {
        if (m->cpu.pp_hit) {
            a64m_log(m, "[stop] probe: first time pc=0x%llx with x%u == 0x%llx (sp=0x%llx):\n", (unsigned long long)m->cpu.pp_pc, m->cpu.pp_reg, (unsigned long long)m->cpu.pp_val, (unsigned long long)m->cpu.pp_sp);
            for (int r = 0; r < 31; r += 4) a64m_log(m, "[stop]  x%-2d=%llx x%-2d=%llx x%-2d=%llx x%-2d=%llx\n", r, (unsigned long long)m->cpu.pp_x[r], r + 1, (unsigned long long)m->cpu.pp_x[r + 1], r + 2 < 31 ? r + 2 : 30, (unsigned long long)m->cpu.pp_x[r + 2 < 31 ? r + 2 : 30], r + 3 < 31 ? r + 3 : 30, (unsigned long long)m->cpu.pp_x[r + 3 < 31 ? r + 3 : 30]);
        } else a64m_log(m, "[stop] probe: pc=0x%llx with x%u == 0x%llx was never reached\n", (unsigned long long)m->cpu.pp_pc, m->cpu.pp_reg, (unsigned long long)m->cpu.pp_val);
    }
    if (m->cpu.pp2_pc) {
        if (m->cpu.pp2_hit) {
            uint64_t t = m->cpu.pp2_x[8], tv = t | 0xffffff8000000000ull;
            a64m_log(m, "[stop] probe2: first time pc=0x%llx with x%u == 0x%llx (sp=0x%llx): x0=%llx x1=%llx x2=%llx x8=%llx (call target stripped: 0x%llx)\n", (unsigned long long)m->cpu.pp2_pc, m->cpu.pp2_reg, (unsigned long long)m->cpu.pp2_val, (unsigned long long)m->cpu.pp2_sp, (unsigned long long)m->cpu.pp2_x[0], (unsigned long long)m->cpu.pp2_x[1], (unsigned long long)m->cpu.pp2_x[2], (unsigned long long)t, (unsigned long long)tv);
            a64m_log(m, "[stop] probe2 callee code:\n"); dump_code_cap(m, tv, tv + 0x50, 0x60);
            { uint64_t o0 = m->cpu.pp2_x[0], q; a64m_log(m, "[stop] probe2 object x0=0x%llx, 32 qwords:", (unsigned long long)o0);
              for (int k = 0; k < 32; k++) { if (peek64_dbg(m, o0 + 8u * (uint64_t)k, &q)) a64m_log(m, " %llx", (unsigned long long)q); else a64m_log(m, " ?"); }
              a64m_log(m, "\n");
              uint64_t arr = 0, prov = 0, vt = 0, tgt = 0;
              if (peek64_dbg(m, o0 + 0x98, &arr) && arr >= 0xffffffe000000000ull) { a64m_log(m, "[stop] probe2 [obj+0x98] array 0x%llx, 8 words:", (unsigned long long)arr); for (int k = 0; k < 4; k++) { if (peek64_dbg(m, arr + 8u * (uint64_t)k, &q)) a64m_log(m, " %llx %llx", (unsigned long long)(q & 0xffffffffull), (unsigned long long)(q >> 32)); else a64m_log(m, " ?"); } a64m_log(m, "\n"); }
              if (peek64_dbg(m, o0 + 0x88, &prov) && prov >= 0xffffffe000000000ull) {
                  a64m_log(m, "[stop] probe2 [obj+0x88] provider 0x%llx, 28 qwords:", (unsigned long long)prov);
                  for (int k = 0; k < 28; k++) { if (peek64_dbg(m, prov + 8u * (uint64_t)k, &q)) a64m_log(m, " %llx", (unsigned long long)q); else a64m_log(m, " ?"); }
                  a64m_log(m, "\n");
                  { static const unsigned offs[3] = { 0xb0, 0xa8, 0xb8 }; for (int t = 0; t < 3; t++) { uint64_t tb = 0; if (peek64_dbg(m, prov + offs[t], &tb) && tb >= 0xffffffe000000000ull) { a64m_log(m, "[stop] probe2 provider table [+0x%x] = 0x%llx, 12 qwords:", offs[t], (unsigned long long)tb); for (int k = 0; k < 12; k++) { if (peek64_dbg(m, tb + 8u * (uint64_t)k, &q)) a64m_log(m, " %llx", (unsigned long long)q); else a64m_log(m, " ?"); } a64m_log(m, "\n"); } else a64m_log(m, "[stop] probe2 provider table [+0x%x] = 0x%llx (not a kernel pointer)\n", offs[t], (unsigned long long)tb); } }
                  { /* 0.4.51 diagnostic: what the provider's pointer fields and the two table trailers point at (names? DT property?) */
                    static const unsigned qi[8] = { 2, 3, 4, 5, 6, 12, 18, 25 };
                    for (int z = 0; z < 8; z++) { uint64_t pq = 0; if (peek64_dbg(m, prov + 8u * qi[z], &pq) && pq >= 0xffffffe000000000ull) { char tg[24]; snprintf(tg, sizeof tg, "prov+0x%x ->", 8u * qi[z]); dump_bytes_va(m, tg, pq, 0x40); } }
                    static const unsigned to[2] = { 0xb0, 0xa8 };
                    for (int z = 0; z < 2; z++) { uint64_t tb = 0, tr = 0; if (peek64_dbg(m, prov + to[z], &tb) && peek64_dbg(m, tb + 0x48, &tr) && tr >= 0xfffffff000000000ull) { char tg[24]; snprintf(tg, sizeof tg, "tbl+0x%x trailer", to[z]); dump_bytes_va(m, tg, tr, 0x80); } }
                    if (arr >= 0xffffffe000000000ull) dump_bytes_va(m, "clock array", arr - 0x40, 0xc0);
                  }
                  if (peek64_dbg(m, prov, &vt)) { vt |= 0xffffff8000000000ull; if (peek64_dbg(m, vt + 0x540, &tgt)) { tgt |= 0xffffff8000000000ull; a64m_log(m, "[stop] probe2 provider vtable 0x%llx, method +0x540 = 0x%llx, code:\n", (unsigned long long)vt, (unsigned long long)tgt); dump_code_cap(m, tgt, tgt + 0x20, 0x30); } }
              } }
        } else a64m_log(m, "[stop] probe2: pc=0x%llx with x%u == 0x%llx was never reached\n", (unsigned long long)m->cpu.pp2_pc, m->cpu.pp2_reg, (unsigned long long)m->cpu.pp2_val);
    }
    if (g_tbl_n) {
        a64m_log(m, "[stop] table at 0x%llx, %llu entries of 0x%llx bytes: words 0..3 and the descriptor behind word 2 (texts where a word points to one):\n", (unsigned long long)g_tbl_va, (unsigned long long)g_tbl_n, (unsigned long long)g_tbl_stride);
        for (uint64_t e = 0; e < g_tbl_n; e++) {
            uint64_t base = g_tbl_va + e * g_tbl_stride, q[4]; char line[700]; int n = 0; bool ok = true;
            for (int k = 0; k < 4; k++) if (!peek64_dbg(m, base + 8u * (uint64_t)k, &q[k])) { ok = false; break; }
            if (!ok) { a64m_log(m, "[stop]  #%llu %llx: (unreadable)\n", (unsigned long long)e, (unsigned long long)base); continue; }
            n += snprintf(line + n, sizeof line - (size_t)n, " #%llu %llx:", (unsigned long long)e, (unsigned long long)base);
            for (int k = 0; k < 4; k++) { char tb[100]; n += snprintf(line + n, sizeof line - (size_t)n, " %llx", (unsigned long long)q[k]); if (text_at(m, q[k], tb, sizeof tb) >= 3) n += snprintf(line + n, sizeof line - (size_t)n, "\"%.60s\"", tb); }
            uint64_t d[4];
            if (q[2] >= 0xfffffff000000000ull && peek64_dbg(m, q[2], &d[0]) && peek64_dbg(m, q[2] + 8, &d[1]) && peek64_dbg(m, q[2] + 16, &d[2]) && peek64_dbg(m, q[2] + 24, &d[3])) {
                n += snprintf(line + n, sizeof line - (size_t)n, " | desc");
                for (int k = 0; k < 4; k++) { char tb[100]; n += snprintf(line + n, sizeof line - (size_t)n, " %llx", (unsigned long long)d[k]); if (text_at(m, d[k], tb, sizeof tb) >= 3) n += snprintf(line + n, sizeof line - (size_t)n, "\"%.60s\"", tb); }
            }
            a64m_log(m, "[stop]%s\n", line);
            { uint64_t x; n = 0; line[0] = 0; n += snprintf(line + n, sizeof line - (size_t)n, "     +0x20..0x58:");
              for (int k = 4; k < 12; k++) { char tb[100]; if (!peek64_dbg(m, base + 8u * (uint64_t)k, &x)) { n += snprintf(line + n, sizeof line - (size_t)n, " ?"); continue; } n += snprintf(line + n, sizeof line - (size_t)n, " %llx", (unsigned long long)x); if (text_at(m, x, tb, sizeof tb) >= 3) n += snprintf(line + n, sizeof line - (size_t)n, "\"%.40s\"", tb); }
              a64m_log(m, "[stop]%s\n", line); }
        }
    }
    for (unsigned w = 0; w < g_diag_n; w++) { a64m_log(m, "[stop] diagnostic code window %u:\n", w); dump_code_cap(m, g_diag_va[w], g_diag_va[w] + g_diag_len[w], g_diag_len[w] + 0x20); dump_strs(m, g_diag_va[w], g_diag_len[w] + 0x20); }
    for (unsigned k = 0; k < 3; k++) {
        if (!g_ddl[k]) continue;
        uint64_t dv = g_ddv[k], dl = g_ddl[k];
        a64m_log(m, "[stop] diagnostic data window %u at kernel virtual 0x%llx (64-bit words, 4 per line):\n", k, (unsigned long long)dv);
        for (uint64_t o = 0; o < dl; o += 32) {
            uint64_t w[4] = {0}, tpa = 0; uint8_t *p = (a64_virt_to_phys(&m->cpu, dv + o, &tpa) == 0) ? a64m_ram_at(m, tpa, 32) : a64m_ram_at(m, m->phys_base + (dv + o - m->virt_base), 32);   /* 0.4.19: through the guest page tables, so zone/aperture addresses work too */
            if (!p) { a64m_log(m, "[stop]  %llx: (outside RAM)\n", (unsigned long long)(dv + o)); break; }
            memcpy(w, p, 32);
            a64m_log(m, "[stop]  %llx (pa %llx): %llx %llx %llx %llx\n", (unsigned long long)(dv + o), (unsigned long long)tpa, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
        }
    }
}
static void dump_watch(A64Machine *m) {
    const A64 *c = &m->cpu;
    if (c->xab_n) {
        a64m_log(m, "[stop] accesses to physical addresses with no RAM and no device: %u, the first was a %s at 0x%llx\n", c->xab_n, c->xab_w ? "write" : "read", (unsigned long long)c->xab_pa);
        for (int k = 0; k < g_soc_n; k++) if (c->xab_pa >= g_soc[k].pa && c->xab_pa - g_soc[k].pa < g_soc[k].size)
            a64m_log(m, "[stop] that address belongs to DeviceTree node '%s' (compatible '%s', base 0x%llx, size 0x%llx; offset +0x%llx)\n", g_soc[k].name, g_soc[k].compat, (unsigned long long)g_soc[k].pa, (unsigned long long)g_soc[k].size, (unsigned long long)(c->xab_pa - g_soc[k].pa));
    }
    { static char lr[3000]; int c2 = a64m_unknown_lastreads(m, lr, sizeof lr); if (c2) a64m_log(m, "[stop] last %d reads from unknown SoC blocks (block+offset=value, oldest first; memory controller polling left out):%s\n", c2, lr); }
    { static char sm[3000]; int cnt = a64m_unknown_summary(m, sm, sizeof sm); if (cnt) a64m_log(m, "[stop] MODELLED unknown SoC blocks the kernel touched (%d; reads/writes, last offset):%s\n", cnt, sm); }
    if (c->at_n) a64m_log(m, "[stop] AT (address translation) instructions executed: %llu, last: va=0x%llx PAR_EL1=0x%llx\n", (unsigned long long)c->at_n, (unsigned long long)c->at_va, (unsigned long long)c->at_par);
    a64m_log(m, "[stop] TPIDR_EL1 writes by the guest: %u%s", c->tp_n, c->tp_n ? " (pc=value):" : " (never written)");
    for (unsigned k = 0; k < c->tp_n; k++) a64m_log(m, " %llx=%llx", (unsigned long long)c->tp_log[k][0], (unsigned long long)c->tp_log[k][1]);
    a64m_log(m, "\n");
    if (c->sw_hi) {
        a64m_log(m, "[stop] stores to watched kernel global (phys 0x%llx.., first %u; size value pc):\n", (unsigned long long)c->sw_lo, c->sw_n);
        for (unsigned k = 0; k < c->sw_n; k++) a64m_log(m, "[stop]  +0x%llx %u 0x%llx pc=0x%llx\n", (unsigned long long)(c->sw[k].addr - c->sw_lo), c->sw[k].sz, (unsigned long long)c->sw[k].val, (unsigned long long)c->sw[k].pc);
        if (c->sw_n >= 3) {
            char w[400]; uint64_t v = c->sw[2].val;
            a64_walk_trace((A64 *)c, v, w, sizeof w);
            a64m_log(m, "[stop] the DT pointer the kernel finally stored, 0x%llx: %s\n", (unsigned long long)v, w);
        }
        if (g_dt_phys) {
            uint8_t *p = a64m_ram_at(m, g_dt_phys, 32);
            if (p) { uint32_t q[8]; memcpy(q, p, 32); a64m_log(m, "[stop] first 8 words at the DT's physical address 0x%llx now: %x %x %x %x %x %x %x %x\n", (unsigned long long)g_dt_phys, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]); }
        }
        for (unsigned k = 3; k-- > 0; ) {   /* latest first: the log can be cut, the interesting store is #2 */
            if (k >= c->sw_n || k != 2) continue;
            a64m_log(m, "[stop] store #%u registers (as of the store):\n", k);
            for (int r = 0; r < 31; r += 4) a64m_log(m, "[stop]  x%d=%llx x%d=%llx x%d=%llx x%d=%llx\n", r, (unsigned long long)c->sw_x[k][r], r+1, (unsigned long long)c->sw_x[k][r+1], r+2, (unsigned long long)c->sw_x[k][r+2], r+3, r+3 < 31 ? (unsigned long long)c->sw_x[k][r+3] : 0ull);
        }
    }
    if (c->vw_val) {
        a64m_log(m, "[stop] stores of the value 0x%llx (8 bytes, or its low half in 4 bytes), first %u (pc va pa size):\n", (unsigned long long)c->vw_val, c->vw_n);
        for (unsigned k = 0; k < c->vw_n; k++) a64m_log(m, "[stop]  pc=%llx va=%llx pa=%llx sz=%u lr=%llx x0-x3=%llx %llx %llx %llx\n", (unsigned long long)c->vw[k].pc, (unsigned long long)c->vw[k].va, (unsigned long long)c->vw[k].pa, c->vw[k].sz, (unsigned long long)c->vw[k].lr, (unsigned long long)c->vw[k].x[0], (unsigned long long)c->vw[k].x[1], (unsigned long long)c->vw[k].x[2], (unsigned long long)c->vw[k].x[3]);
    }
    for (int i = 0; i < 2; i++) if (c->swb[i].hi) {
        a64m_log(m, "[stop] stores to kernel global phys 0x%llx (first %u; value pc):", (unsigned long long)c->swb[i].lo, c->swb[i].n);
        for (unsigned k = 0; k < c->swb[i].n; k++) a64m_log(m, " %llx@%llx", (unsigned long long)c->swb[i].e[k].val, (unsigned long long)c->swb[i].e[k].pc);
        uint8_t *p = a64m_ram_at(m, c->swb[i].lo, 8); uint64_t cur = 0; if (p) memcpy(&cur, p, 8);
        a64m_log(m, " | now %llx\n", (unsigned long long)cur);
    }
    if (c->tr_pc) {
        a64m_log(m, "[stop] the %u instructions executed from the first execution of pc 0x%llx on (pc only, 8 per line):\n", c->tr_n, (unsigned long long)c->tr_pc);
        for (unsigned q = 0; q < c->tr_n; q += 8) {
            char ln[200]; int o = 0;
            for (unsigned r = q; r < q + 8 && r < c->tr_n; r++) o += snprintf(ln + o, sizeof ln - o, " %llx", (unsigned long long)c->tr_pcs[r]);
            a64m_log(m, "[stop] %s\n", ln);
        }
    }
    if (c->ctr_pc && c->ctr_mode == 1) {
        unsigned tot = c->ctr_total, n = tot < 40 ? tot : 40;
        a64m_log(m, "[stop] the last %u of %u executions of pc 0x%llx (x0 x8 x9 x10 x24 x25 x26 x27), oldest first:\n", n, tot, (unsigned long long)c->ctr_pc);
        for (unsigned q = 0; q < n; q++) { unsigned k2 = tot < 40 ? q : (tot + q) % 40;
            a64m_log(m, "[stop]  #%u %llx %llx %llx %llx %llx %llx %llx %llx\n", tot - n + q, (unsigned long long)c->ctr[k2][0], (unsigned long long)c->ctr[k2][1], (unsigned long long)c->ctr[k2][2], (unsigned long long)c->ctr[k2][3], (unsigned long long)c->ctr[k2][4], (unsigned long long)c->ctr[k2][5], (unsigned long long)c->ctr[k2][6], (unsigned long long)c->ctr[k2][7]); }
    } else if (c->ctr_pc) {
        a64m_log(m, "[stop] calls to 0x%llx (only delta!=0, first %u; x0 x1 x2 x3 x4 x5 x6 lr):\n", (unsigned long long)c->ctr_pc, c->ctr_n);
        for (unsigned k = 0; k < c->ctr_n; k++) a64m_log(m, "[stop]  #%u %llx %llx %llx %llx %llx %llx %llx lr=%llx\n", k, (unsigned long long)c->ctr[k][0], (unsigned long long)c->ctr[k][1], (unsigned long long)c->ctr[k][2], (unsigned long long)c->ctr[k][3], (unsigned long long)c->ctr[k][4], (unsigned long long)c->ctr[k][5], (unsigned long long)c->ctr[k][6], (unsigned long long)c->ctr[k][7]);
    }
    if (!c->watch_n || !getenv("A64_DUMP_BOOTARGS_WATCH")) return;   /* boot_args accesses are known; omitted to keep the log short */
    a64m_log(m, "[stop] guest accesses to boot_args (first %u; offset size R/W value pc):\n", c->watch_n);
    for (unsigned k = 0; k < c->watch_n; k++)
        a64m_log(m, "[stop]  +0x%llx %u %s 0x%llx pc=0x%llx\n", (unsigned long long)(c->watch[k].addr - c->watch_lo), c->watch[k].sz, c->watch[k].wr ? "W" : "R", (unsigned long long)c->watch[k].val, (unsigned long long)c->watch[k].pc);
    for (unsigned k = 0; k < c->watch_n; k++) if (c->watch[k].addr - c->watch_lo >= 96 && c->watch[k].addr - c->watch_lo < 104 && !c->watch[k].wr) { a64m_log(m, "[stop] code around the first read of deviceTreeP:\n"); dump_code(m, c->watch[k].pc - 0x30, c->watch[k].pc + 0x30); break; }
}
static void dump_impl(A64Machine *m) {   /* 0.4.71: the Apple registers used, in one line (was one log line per first read/write) */
    if (!m->impl_n) return;
    char b[2600]; size_t n = 0; b[0] = 0;
    for (unsigned i = 0; i < m->impl_n && n + 80 < sizeof b; i++) { unsigned k = m->impl_key[i];
        n += (size_t)snprintf(b + n, sizeof b - n, " S3_%u_C%u_C%u_%u(%s%s)=0x%llx", (k >> 11) & 7, (k >> 7) & 15, (k >> 3) & 15, k & 7, (m->impl_seen[i] & 1) ? "r" : "", (m->impl_seen[i] & 2) ? "w" : "", (unsigned long long)m->impl_val[i]); }
    a64m_log(m, "[stop] MODELLED Apple registers the kernel used (r = read, w = written; each reads back the last value written, shown here):%s\n", b);
    for (unsigned j = 0; j < sizeof IMPL_READY / sizeof *IMPL_READY; j++) for (unsigned i = 0; i < m->impl_n; i++) if (m->impl_key[i] == IMPL_READY[j].key && (m->impl_seen[i] & 4u))
        a64m_log(m, "[stop]   ASSUMPTION: bits 0x%llx of that register read as 1 (%s)\n", (unsigned long long)IMPL_READY[j].force_bits, IMPL_READY[j].why);
}
static void dump_regs(A64Machine *m) {
    const A64 *c = &m->cpu;
    for (int i = 0; i < 31; i += 4) {
        char b[160]; int n = 0;
        for (int k = i; k < i + 4 && k < 31; k++) n += snprintf(b + n, sizeof b - (size_t)n, "x%-2d=%016llx ", k, (unsigned long long)c->x[k]);
        a64m_log(m, "[stop] %s\n", b);
    }
    a64m_log(m, "[stop] sp=%016llx pc=%016llx el=%d daif=%x NZCV=%d%d%d%d\n", (unsigned long long)c->sp, (unsigned long long)c->pc, c->el, c->daif, c->n, c->z, c->c, c->v);
    if (c->mode == A64_MODE_SYSTEM) {
        a64m_log(m, "[stop] SCTLR_EL1=%llx TCR_EL1=%llx TTBR0_EL1=%llx TTBR1_EL1=%llx VBAR_EL1=%llx\n", (unsigned long long)c->sctlr_el1, (unsigned long long)c->tcr_el1,
                 (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->vbar_el1);
        a64m_log(m, "[stop] ESR_EL1=%llx ELR_EL1=%llx FAR_EL1=%llx\n", (unsigned long long)c->esr_el1, (unsigned long long)c->elr_el1, (unsigned long long)c->far_el1);
        char w[400];
        a64_walk_trace((A64 *)c, c->far_el1, w, sizeof w); a64m_log(m, "[stop] %s\n", w);
        a64_walk_trace((A64 *)c, c->elr_el1, w, sizeof w); a64m_log(m, "[stop] %s\n", w);
        for (int k = 0; k < 2; k++) {                       /* raw words at the low bits-cleared TTBR, to spot alignment assumptions */
            uint64_t base = (k ? c->ttbr1_el1 : c->ttbr0_el1) & 0x0000FFFFFFFFFFC0ull;
            uint8_t *t = a64m_ram_at(m, base, 64);
            if (!t) { a64m_log(m, "[stop] TTBR%d base 0x%llx is outside RAM\n", k, (unsigned long long)base); continue; }
            char b[260]; int n = snprintf(b, sizeof b, "[stop] first 8 words at TTBR%d&~0x3f (0x%llx):", k, (unsigned long long)base);
            for (int e = 0; e < 8; e++) { uint64_t d; memcpy(&d, t + e * 8, 8); n += snprintf(b + n, sizeof b - (size_t)n, " %llx", (unsigned long long)d); }
            a64m_log(m, "%s\n", b);
        }
    }
    dump_impl(m);
}
static bool mmu_on(const A64Machine *m) { return m->cpu.mode == A64_MODE_SYSTEM && (m->cpu.sctlr_el1 & 1); }
/* a = guest address as the CPU sees it (virtual when the MMU is on, physical otherwise) */
static bool read_word(A64Machine *m, uint64_t a, uint32_t *x) {
    uint64_t pa = a;
    if (mmu_on(m) && a64_virt_to_phys(&m->cpu, a, &pa) != 0) return false;
    uint8_t *w = a64m_ram_at(m, pa, 4); if (!w) return false;
    memcpy(x, w, 4); return true;
}
static uint64_t kvirt(const A64Machine *m, uint64_t a) { return mmu_on(m) ? a : a - m->phys_base + m->virt_base; }
static void dump_code_cap(A64Machine *m, uint64_t lo, uint64_t hi, uint64_t cap) {
    lo = (lo & ~3ull) - 16; hi = (hi & ~3ull) + 20;
    if (hi - lo > cap) hi = lo + cap;
    for (uint64_t a = lo; a <= hi;) {
        char b[160]; int n = snprintf(b, sizeof b, "[stop] code 0x%llx (kernel virtual 0x%llx):", (unsigned long long)a, (unsigned long long)kvirt(m, a));
        for (int k = 0; k < 4 && a <= hi; k++, a += 4) { uint32_t x = 0; if (read_word(m, a, &x)) n += snprintf(b + n, sizeof b - (size_t)n, " %08x", x); else n += snprintf(b + n, sizeof b - (size_t)n, " ????????"); }
        a64m_log(m, "%s\n", b);
    }
}
static void dump_code(A64Machine *m, uint64_t lo, uint64_t hi) { dump_code_cap(m, lo, hi, 0x80); }
static bool stuck_check(A64Machine *m) {
    uint64_t pc = m->cpu.pc;
    if (m->calm == 0) { m->pmin = m->pmax = pc; m->calm_exc = m->cpu.exc_count; m->calm = 1; return false; }
    if (pc < m->pmin) m->pmin = pc;
    if (pc > m->pmax) m->pmax = pc;
    if (m->pmax - m->pmin > STUCK_WINDOW) { m->pmin = m->pmax = pc; m->calm = 1; return false; }
    return ++m->calm >= STUCK_CHUNKS;
}


/* 0.4.66: the kernel's own printf text. With a framebuffer in boot_args XNU's verbose-boot text goes to the screen (and its log buffer), not to a serial
 * port, so the Kernel tab stayed empty until the panic. XNU keeps everything printf'd in a ring buffer, `struct msgbuf` (bsd/sys/msgbuf.h as I know it:
 * int magic 0x063061, int size, int bufx (write index), int bufr, char *bufc). MODELLED: this finds that header in guest RAM by its magic and mirrors the new
 * bytes into the guest log. It reads guest memory only; nothing is invented. ASSUMPTION: the layout above is right for iOS 12; the result is validated
 * (size, index, a kernel pointer that translates to RAM) and nothing is shown if no header passes. */
static void msgbuf_scan(A64Machine *m) {
    const uint32_t *w = (const uint32_t *)m->ram; uint64_t nw = m->ram_size / 4;
    for (uint64_t i = 0; i + 6 <= nw; i += 2) {
        if (w[i] != 0x063061u) continue;
        uint32_t size = w[i + 1], bufx = w[i + 2], bufr = w[i + 3];
        if (size < 0x1000 || size > 0x400000 || bufx >= size || bufr > size) continue;
        uint64_t bc = (uint64_t)w[i + 4] | ((uint64_t)w[i + 5] << 32), pa = 0;
        if (bc == 0 || a64_virt_to_phys(&m->cpu, bc, &pa) != 0 || !a64m_ram_at(m, pa, size)) continue;
        m->mb.hdr_pa = m->ram_base + i * 4; m->mb.buf_pa = pa; m->mb.size = size; m->mb.last = 0; m->mb.found = 1;
        a64m_log(m, "[emu] MODELLED: kernel message buffer (msgbuf) found: header at physical 0x%llx, text at 0x%llx, %u bytes. Its text is mirrored into the Kernel tab (ASSUMPTION: struct layout as in XNU sources; read-only, nothing invented)\n",
                 (unsigned long long)m->mb.hdr_pa, (unsigned long long)pa, size);
        return;
    }
}
static void mb_emit(A64Machine *m, const uint8_t *b, uint32_t n) {
    char t[512]; uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) { if (b[i]) t[k++] = (char)b[i]; if (k == sizeof t || i + 1 == n) { if (k) log_raw(m, t, k); k = 0; } }
}
void a64m_msgbuf_sync(A64Machine *m) {
    if (!m->mb.found) { msgbuf_scan(m); if (!m->mb.found) return; }
    const uint32_t *h = (const uint32_t *)a64m_ram_at(m, m->mb.hdr_pa, 8 * 3); const uint8_t *b = a64m_ram_at(m, m->mb.buf_pa, m->mb.size);
    if (!h || !b || h[0] != 0x063061u) return;
    uint32_t x = h[2]; if (x >= m->mb.size) return;
    if (x >= m->mb.last) mb_emit(m, b + m->mb.last, x - m->mb.last);
    else { mb_emit(m, b + m->mb.last, m->mb.size - m->mb.last); mb_emit(m, b, x); }
    m->mb.last = x;
}
static double g_t0 = 0;   /* 0.4.91: wall clock at the first a64m_run, for the speed line */
static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static void finish(A64Machine *m, A64MState st) {
    if (m->attempt) a64m_msgbuf_sync(m);
    m->state = st;
    if (m->reported_end) return;
    m->reported_end = true;
    if (st == A64M_FAULT) {
        char hist[A64_PC_HIST * 20 + 8];
        a64_format_pc_history(&m->cpu, hist, sizeof hist);
        a64m_log(m, "\n[emu] EMULATOR FAULT: %s\n[emu] last PCs: %s\n[emu] el=%d steps=%llu exceptions=%llu esr_el1=0x%llx elr_el1=0x%llx far_el1=0x%llx\n",
                 m->cpu.err, hist, m->cpu.el, (unsigned long long)m->cpu.steps, (unsigned long long)m->cpu.exc_count,
                 (unsigned long long)m->cpu.esr_el1, (unsigned long long)m->cpu.elr_el1, (unsigned long long)m->cpu.far_el1);
        { double dt = g_t0 ? mono_s() - g_t0 : 0; if (dt > 0.001) a64m_log(m, "[emu] speed: %.1f s wall clock since the first run call, %.2f million guest instructions per second on average (includes time spent drawing/idle in the app)\n", dt, (double)m->cpu.steps / dt / 1e6); }
        if (m->attempt) {
            const A64 *c = &m->cpu; uint64_t pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST];
            uint32_t insn = 0; read_word(m, pc, &insn);
            a64m_log(m, "[stop] last executed pc=0x%llx (kernel virtual 0x%llx), instruction word 0x%08x, x0=0x%llx x1=0x%llx lr=0x%llx sp=0x%llx\n",
                     (unsigned long long)pc, (unsigned long long)kvirt(m, pc), insn,
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30], (unsigned long long)c->sp);
            if (!m->stop_dumped) { dump_regs(m); dump_diag(m); dump_strrefs(m); dump_refs(m); dump_callers(m); dump_exceptions(m); dump_watch(m); dump_code(m, pc, pc); }   /* 0.4.71: not printed a second time after the stuck-loop report */
        }
        if (m->raw) a64m_log(m, "[emu] hint: a raw image that faults within a few instructions is usually not AArch64 code at all (32-bit ARM, compressed, encrypted or wrapped in a container). Open the Diagnostics tab and inspect the file.\n");
    } else if (st == A64M_POWEROFF) {
        a64m_log(m, "\n[emu] guest finished, exit code %d, %llu instructions\n", m->cpu.exit_code, (unsigned long long)m->cpu.steps);
    } else if (st == A64M_STOPPED) {
        a64m_log(m, "\n[emu] stopped by user after %llu instructions\n", (unsigned long long)m->cpu.steps);
        if (m->attempt) {
            const A64 *c = &m->cpu; char hist[A64_PC_HIST * 20 + 8];
            uint64_t pc = c->pc_hist[(c->pc_hist_i + A64_PC_HIST - 1) % A64_PC_HIST];
            a64m_log(m, "[emu] where it was: pc=0x%llx (kernel virtual 0x%llx) lr=0x%llx sp=0x%llx\n",
                     (unsigned long long)pc, (unsigned long long)kvirt(m, pc), (unsigned long long)c->x[30], (unsigned long long)c->sp);
            a64_format_pc_history(c, hist, sizeof hist);
            a64m_log(m, "[emu] last pcs: %s\n", hist);
            a64m_log(m, "[emu] timers: counter=%llu CNTP ctl=%llx cval=%llu | CNTV ctl=%llx cval=%llu | exceptions=%llu FIQs=%llu daif=%x\n",
                     (unsigned long long)a64_counter(c), (unsigned long long)c->cntp_ctl, (unsigned long long)c->cntp_cval,
                     (unsigned long long)c->cntv_ctl, (unsigned long long)c->cntv_cval, (unsigned long long)c->exc_count, (unsigned long long)c->fiq_count, (unsigned)c->daif);
            { uint64_t fp = c->x[29]; char fb[400]; size_t n = 0; fb[0] = 0;
              for (int k = 0; k < 12 && fp && n < sizeof fb - 40; k++) { uint64_t nfp, lr; if (!peek64_dbg(m, fp, &nfp) || !peek64_dbg(m, fp + 8, &lr)) break; n += (size_t)snprintf(fb + n, sizeof fb - n, " %llx", (unsigned long long)lr); if (nfp <= fp) break; fp = nfp; }
              a64m_log(m, "[emu] call chain (x29 frames, return addresses):%s\n", fb); }
            dump_regs(m); dump_diag(m);
        }
    }
}

A64MState a64m_run(A64Machine *m, uint64_t max_steps, bool realtime) {
    if (g_t0 == 0) g_t0 = mono_s();
    if (m->state == A64M_POWEROFF || m->state == A64M_FAULT || m->state == A64M_STOPPED) return m->state;
    uint64_t done = 0;
    m->state = A64M_RUNNING;
    while (done < max_steps) {
        if (m->stop) { finish(m, A64M_STOPPED); return m->state; }
        uint64_t chunk = max_steps - done; if (chunk > 20000) chunk = 20000;
        uint64_t before = m->cpu.steps;
        int r = a64_run(&m->cpu, chunk);
        done += m->cpu.steps - before;
        if (m->attempt && m->cpu.steps >= m->mb.next) { a64m_msgbuf_sync(m); m->mb.next = m->cpu.steps + (m->mb.found ? 2000000ull : 40000000ull); }
        if (r == A64_RUN_FAULT) { finish(m, A64M_FAULT); return m->state; }
        if (m->attempt && r == A64_RUN_LIMIT && stuck_check(m)) {
            pthread_mutex_lock(&m->lock); size_t gout = m->slen[1]; pthread_mutex_unlock(&m->lock);
            if (m->cpu.exc_count)
                snprintf(m->cpu.err, sizeof m->cpu.err, "the guest spins in a tight loop AFTER an exception (pc stayed within 0x%llx..0x%llx for %d million instructions, guest output so far: %zu bytes): "
                         "this is the guest's own exception/panic handler, not a wait for hardware - the cause is the first exception listed below",
                         (unsigned long long)m->pmin, (unsigned long long)m->pmax, STUCK_CHUNKS * 20000 / 1000000, gout);
            else
                snprintf(m->cpu.err, sizeof m->cpu.err, "guest is stuck in a tight loop (pc stayed within 0x%llx..0x%llx for %d million instructions, guest output so far: %zu bytes): "
                         "almost certainly waiting for hardware or a state that a64virt does not provide",
                         (unsigned long long)m->pmin, (unsigned long long)m->pmax, STUCK_CHUNKS * 20000 / 1000000, gout);
            m->cpu.fault = 1;
            a64m_log(m, "\n[stop] loop window virtual 0x%llx..0x%llx\n", (unsigned long long)kvirt(m, m->pmin), (unsigned long long)kvirt(m, m->pmax));
            dump_code(m, m->cpu.pc, m->cpu.pc);
            dump_regs(m); dump_diag(m); dump_strrefs(m); dump_refs(m); dump_callers(m); dump_exceptions(m); dump_watch(m);
            if (m->cpu.exc_count)
                a64m_log(m, "[stop] the loop follows an exception: ESR_EL1=0x%llx ELR_EL1=0x%llx (kernel virtual 0x%llx) VBAR_EL1=0x%llx cause: %s\n"
                         "[stop] i.e. the guest hit something this emulator lacks, installed its own handler, and that handler just spins.\n",
                         (unsigned long long)m->cpu.esr_el1, (unsigned long long)m->cpu.elr_el1, (unsigned long long)kvirt(m, m->cpu.elr_el1),
                         (unsigned long long)m->cpu.vbar_el1, m->cpu.trap_note[0] ? m->cpu.trap_note : "unknown (see ESR_EL1)");
            m->stop_dumped = true; finish(m, A64M_FAULT); return m->state;
        }
        if (r == A64_RUN_HALTED) { finish(m, A64M_POWEROFF); return m->state; }
        if (r == A64_RUN_IDLE) {
            uint64_t t = m->cpu.idle_ticks;
            if (!realtime) {
                if (t) { a64_advance_time(&m->cpu, t); m->cpu.idle_ticks = 0; }
                else { m->state = A64M_IDLE; return m->state; }   /* nothing can ever wake it */
            } else {
                uint64_t us = t ? t * 1000000ull / A64_TIMER_HZ : 5000;
                if (us > 5000) us = 5000;
                if (us) { struct timespec ts = { 0, (long)(us * 1000) }; nanosleep(&ts, NULL); }
                if (t) { uint64_t adv = us * A64_TIMER_HZ / 1000000ull; if (!adv) adv = 1; a64_advance_time(&m->cpu, adv); }
                done += 1;      /* keep the loop responsive to stop requests */
            }
        }
    }
    return m->state;
}
