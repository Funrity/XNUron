/* XNUron test guest kernel.  Our own bare-metal software for the a64virt machine:
 * it exercises exceptions, EL0<->EL1, SVC, timer IRQ, MMIO IRQ, the display and the
 * touch device through the emulated CPU.  It is NOT iOS and contains no Apple code. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef long long s64;

#define UART   ((volatile u32 *)0x09000000ull)
#define FB     ((volatile u32 *)0x0A000000ull)
#define TOUCH  ((volatile u32 *)0x0A001000ull)
#define SYSCON ((volatile u32 *)0x0A002000ull)
#define W 270
#define H 480

#define SYSREG_R(name) ({ u64 v_; __asm__ volatile("mrs %0, " #name : "=r"(v_)); v_; })
#define SYSREG_W(name, v) __asm__ volatile("msr " #name ", %0" :: "r"((u64)(v)))

void *memset(void *d, int v, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)v; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }

static void putc_(char c) { *UART = (u32)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putx(u64 v) { puts_("0x"); int started = 0; for (int s = 60; s >= 0; s -= 4) { int d = (v >> s) & 15; if (d || started || !s) { putc_("0123456789abcdef"[d]); started = 1; } } }
static void putd(u64 v) { char b[24]; int n = 0; do { b[n++] = '0' + v % 10; v /= 10; } while (v); while (n) putc_(b[--n]); }
static void stamp(void) {
    u64 t = SYSREG_R(cntvct_el0) / 24;           /* microseconds of virtual time */
    puts_("["); putd(t / 1000000); putc_('.');
    u64 us = t % 1000000; for (u64 d = 100000; d; d /= 10) putc_('0' + (us / d) % 10);
    puts_("] ");
}
static void log_(const char *s) { stamp(); puts_(s); putc_('\n'); }
static void logx(const char *s, u64 v) { stamp(); puts_(s); putx(v); putc_('\n'); }

static int failures;
static void check(const char *name, int ok) {
    stamp(); puts_("[test] "); puts_(name); puts_(ok ? ": PASS\n" : ": FAIL\n");
    if (!ok) failures++;
}

/* ---------------------------------------------------------------- exceptions */
static volatile u64 n_svc_el1, n_dabt, n_undef, n_timer, n_touch, n_el0_svc, n_el0_trap, last_esr, last_far;
static volatile u64 ticks_wanted; static volatile u64 interactive;
extern void el0_return(void);
extern void enter_el0(u64 entry, u64 sp);
static void gfx_on_tick(void); static void gfx_on_touch(void);

void exc_handler(u64 *frame, u64 vec) {
    u64 esr = SYSREG_R(esr_el1), ec = esr >> 26, elr = SYSREG_R(elr_el1);
    u64 kind = vec & 3, grp = vec >> 2;           /* grp 0: cur EL SP0, 1: cur EL SPx, 2: lower EL */
    if (kind == 1) {                              /* IRQ */
        if (SYSREG_R(cntv_ctl_el0) & 4) {
            SYSREG_W(cntv_ctl_el0, 2);            /* mask + disable until re-armed */
            n_timer++; if (interactive) gfx_on_tick();
            if (ticks_wanted) { SYSREG_W(cntv_tval_el0, 24000000 / 100); SYSREG_W(cntv_ctl_el0, 1); }
        }
        if (TOUCH[5] & 1) { TOUCH[5] = 1; n_touch++; if (interactive) gfx_on_touch(); }
        return;
    }
    last_esr = esr;
    if (kind != 0) { log_("[exc] unexpected SError/FIQ vector"); *SYSCON = 2; }
    if (ec == 0x15) {                             /* SVC */
        u64 imm = esr & 0xFFFF;
        if (grp == 2) {
            n_el0_svc++;
            if (imm == 0xFF) { SYSREG_W(elr_el1, (u64)el0_return); SYSREG_W(spsr_el1, 0x3C5); return; }
            frame[0] += 1;                        /* the "syscall" returns x0 + 1 */
        } else n_svc_el1++;
        return;
    }
    if (ec == 0x24 || ec == 0x25) { n_dabt++; last_far = SYSREG_R(far_el1); SYSREG_W(elr_el1, elr + 4); return; }
    if (ec == 0x18 && grp == 2) { n_el0_trap++; SYSREG_W(elr_el1, elr + 4); return; }
    if (ec == 0x00) { n_undef++; SYSREG_W(elr_el1, elr + 4); return; }
    stamp(); puts_("[exc] unhandled exception ec="); putx(ec); puts_(" elr="); putx(elr); putc_('\n');
    *SYSCON = 3;
}

/* ------------------------------------------------------------------- tests */
static u64 fact(u64 n) { return n <= 1 ? 1 : n * fact(n - 1); }
static u32 popc(u64 v) { u32 c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

static void test_cpu(void) {
    check("integer: 15! == 1307674368000", fact(15) == 1307674368000ull);
    check("integer: popcount", popc(0xF0F0F0F0F0F0F0F0ull) == 32);
    volatile u64 a = 0xFFFFFFFFFFFFFFFFull, b = 0xFFFFFFFFFFFFFFFFull;
    check("integer: 128-bit multiply high", (u64)(((unsigned __int128)a * b) >> 64) == 0xFFFFFFFFFFFFFFFEull);
    volatile s64 x = -7, y = 2;
    check("integer: signed division", x / y == -3 && x % y == -1);
    u8 buf[64]; for (int i = 0; i < 64; i++) buf[i] = (u8)i; memcpy(buf + 32, buf, 32);
    check("memory: memcpy 32 bytes", buf[32] == 0 && buf[33] == 1 && buf[63] == 31 && buf[31] == 31);
    volatile u32 w = 5; __atomic_fetch_add(&w, 37, __ATOMIC_SEQ_CST);
    check("atomics: ldxr/stxr fetch_add", w == 42);
    u32 old = 42, nv = 7; int ok = __atomic_compare_exchange_n(&w, &old, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    check("atomics: compare_exchange", ok && w == 7);
}

static void test_exceptions(void) {
    u64 before = n_svc_el1;
    __asm__ volatile("svc #1");
    check("exception: SVC at EL1 taken and returned", n_svc_el1 == before + 1 && (last_esr >> 26) == 0x15 && (last_esr & 0xFFFF) == 1);
    before = n_dabt;
    volatile u64 *bad = (volatile u64 *)0x10; u64 junk = *bad; (void)junk;
    check("exception: data abort on unmapped address (FAR/ESR)", n_dabt == before + 1 && last_far == 0x10 && (last_esr >> 26) == 0x25);
    before = n_undef;
    __asm__ volatile(".inst 0x00000000");
    check("exception: undefined instruction (UDF)", n_undef == before + 1);
    check("system registers: CurrentEL == EL1", SYSREG_R(CurrentEL) == 4);
    check("system registers: CNTFRQ_EL0 == 24 MHz", SYSREG_R(cntfrq_el0) == 24000000);
    SYSREG_W(tpidr_el1, 0x1234); check("system registers: TPIDR_EL1 round trip", SYSREG_R(tpidr_el1) == 0x1234);
}

static void test_timer(void) {
    u64 t0 = n_timer; ticks_wanted = 1;
    SYSREG_W(cntv_tval_el0, 24000000 / 100); SYSREG_W(cntv_ctl_el0, 1);
    __asm__ volatile("msr daifclr, #2");
    while (n_timer < t0 + 3) __asm__ volatile("wfi");
    ticks_wanted = 0; __asm__ volatile("msr daifset, #2");
    check("timer: three IRQs at 100 Hz delivered via WFI", n_timer >= t0 + 3);
}

static void el0_main(void) {
    u64 r;
    __asm__ volatile("mov x0, #41\n svc #0x10\n mov %0, x0" : "=r"(r) :: "x0");
    u64 sctlr; __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));        /* must trap */
    u64 cnt; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));           /* allowed at EL0 */
    __asm__ volatile("svc #0x11");
    if (r == 42 && cnt) __asm__ volatile("svc #0x12"); /* marker: EL0 saw the return value */
    __asm__ volatile("svc #0xFF");
    for (;;) {}
}
static u8 el0_stack[4096] __attribute__((aligned(16)));

static void test_el0(void) {
    u64 s0 = n_el0_svc, t0 = n_el0_trap;
    enter_el0((u64)el0_main, (u64)(el0_stack + sizeof el0_stack));
    check("EL0: syscalls via SVC reach the EL1 handler (4 SVCs, x0 returned)", n_el0_svc == s0 + 4);
    check("EL0: privileged MRS SCTLR_EL1 traps to EL1 (EC=0x18)", n_el0_trap == t0 + 1);
}

/* ---------------------------------------------------------------- graphics */
static u32 fb[W * H] __attribute__((aligned(4096)));
static void rect(int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h && j < H; j++) if (j >= 0) for (int i = x; i < x + w && i < W; i++) if (i >= 0) fb[j * W + i] = c;
}
static void flip(void) { FB[6] = 1; }
static void gfx_init(void) {
    u64 a = (u64)fb; FB[2] = (u32)a; FB[3] = (u32)(a >> 32); FB[4] = 1;
    for (int y = 0; y < H; y++) {
        u32 r = 16 + y * 40 / H, g = 24 + y * 60 / H, b = 60 + y * 120 / H;
        u32 c = (r << 16) | (g << 8) | b; for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }
    rect(0, 0, W, 28, 0x00101018);                     /* status strip */
    rect(8, 8, 60, 12, 0x0055AAFF);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {   /* "tiles" of the test UI */
        u32 c = 0x00203050 + ((i * 4 + j) * 0x0B0804); rect(18 + i * 62, 60 + j * 70, 50, 50, c);
    }
    rect(10, H - 40, W - 20, 30, 0x00202830);
    flip();
}
static u32 hb;
static void gfx_on_tick(void) {
    static int div; if (++div < 25) return; div = 0;    /* 100 Hz / 25 = 4 Hz heartbeat */
    hb ^= 1; rect(W - 24, 8, 12, 12, hb ? 0x0040E060 : 0x00104020); flip();
}
static u32 last_x = 0xFFFF, last_y;
static void gfx_on_touch(void) {
    u32 x = TOUCH[0], y = TOUCH[1], down = TOUCH[2] & 1;
    stamp(); puts_("[touch] "); puts_(down ? "down/move x=" : "up x="); putd(x); puts_(" y="); putd(y); putc_('\n');
    if (down) { rect((int)x - 5, (int)y - 5, 11, 11, 0x00FFD040); last_x = x; last_y = y; }
    else if (last_x != 0xFFFF) { rect((int)last_x - 5, (int)last_y - 5, 11, 11, 0x00FF4060); }
    flip();
}

void kmain(u64 flags) {
    log_("XNUron test guest kernel (bare-metal AArch64 on a64virt)");
    log_("NOTE: this is our own test software. It is NOT iOS and contains no Apple code.");
    logx("CurrentEL = ", SYSREG_R(CurrentEL));
    logx("MIDR_EL1  = ", SYSREG_R(midr_el1));
    logx("boot flags = ", flags);
    test_cpu(); test_exceptions(); test_timer(); test_el0();
    stamp(); puts_("[test] summary: "); putd(failures); puts_(" failure(s)\n");
    if (!(flags & 1)) {
        log_(failures ? "self-test FAILED, powering off" : "self-test OK, powering off");
        *SYSCON = failures ? 1 : 0;
        for (;;) __asm__ volatile("wfi");
    }
    gfx_init(); interactive = 1; ticks_wanted = 1;
    TOUCH[4] = 1;                                   /* touch IRQ enable */
    log_("display initialised (270x480 XRGB8888); entering interactive loop");
    SYSREG_W(cntv_tval_el0, 24000000 / 100); SYSREG_W(cntv_ctl_el0, 1);
    __asm__ volatile("msr daifclr, #2");
    for (;;) __asm__ volatile("wfi");
}
