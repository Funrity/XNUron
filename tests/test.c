#include "../app/src/main/cpp/arm64.h"
#include "../app/src/main/cpp/a64asm.h"
#include "../app/src/main/cpp/a64demo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char outbuf[4096]; static size_t outn;
static void sink(void *ud, const char *b, size_t n) { (void)ud; memcpy(outbuf + outn, b, n); outn += n; outbuf[outn] = 0; }
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

int main(void) {
    static uint8_t mem[0x10000]; A64 c;

    /* 1. full demo */
    a64_init(&c, mem, 0, sizeof mem); c.out = sink; outn = 0; outbuf[0] = 0;
    a64_demo_load(&c);
    int rc = a64_run(&c, 1000000);
    printf("--- demo output ---\n%s--- rc=%d steps=%llu ---\n", outbuf, rc, (unsigned long long)c.steps);
    CHECK(rc == 0 && c.exit_code == 0, "demo exits cleanly via svc #0x80");
    CHECK(strstr(outbuf, "sum(1..100) = 5050\n") != NULL, "loop + udiv/msub decimal print = 5050");
    CHECK(strstr(outbuf, "= 3628800\n") != NULL, "recursive factorial 10! = 3628800");

    /* 2. flags / 32-bit wrap: w0 = 0xFFFFFFFF; w0 += 1 -> 0 ; cmp -> Z */
    static Asm a; memset(&a, 0, sizeof a);
    a_movz(&a, 0, 0xFFFF); a_movk(&a, 0, 0xFFFF, 1);       /* x0 = 0xFFFFFFFF */
    a_e(&a, 0x11000400u);                                  /* add w0, w0, #1 */
    a_e(&a, 0x7100001Fu);                                  /* cmp w0, #0 */
    a_movz(&a, 8, 93); a_svc(&a, 0); a_finish(&a);
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000;
    rc = a64_run(&c, 100);
    CHECK(rc == 0 && c.x[0] == 0 && c.z, "32-bit add wraps to 0 and sets Z");

    /* 3. fault on bad memory */
    memset(&a, 0, sizeof a);
    a_movz(&a, 1, 0xFFFF); a_movk(&a, 1, 0xFFFF, 1); a_ldr(&a, 0, 1, 0); a_finish(&a);
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000;
    rc = a64_run(&c, 100);
    CHECK(rc == -1 && c.fault, "load from unmapped address faults");

    /* 4. step limit on infinite loop (b .) */
    memset(&a, 0, sizeof a); a_label(&a, 0); a_b(&a, 0); a_finish(&a);
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000;
    CHECK(a64_run(&c, 1000) == 1, "infinite loop stops at step limit");

    /* 5. 0.4.354 MODELLED hook: at hk_pc the bytes [x19+0x10b]=1 and [x19+0x10c]=0 are stored before the instruction executes */
    memset(&a, 0, sizeof a);
    a_movz(&a, 19, 0x2000);
    a_e(&a, 0x39400000u | (0x10bu << 10) | (19u << 5) | 10u);   /* ldrb w10, [x19, #0x10b] */   /* hook pc = this instruction */
    a_e(&a, 0x39400000u | (0x10cu << 10) | (19u << 5) | 11u);   /* ldrb w11, [x19, #0x10c] */
    a_movz(&a, 8, 93); a_svc(&a, 0); a_finish(&a);
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000; mem[0x2000 + 0x10c] = 5; c.hk_pc = 0x1004;
    rc = a64_run(&c, 100);
    CHECK(rc == 0 && c.x[10] == 1 && c.x[11] == 0 && c.hk_n == 1, "hook stores [x19+0x10b]=1 and [x19+0x10c]=0 once");
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000; mem[0x2000 + 0x10c] = 5;
    rc = a64_run(&c, 100);
    CHECK(rc == 0 && c.x[10] == 0 && c.x[11] == 5 && c.hk_n == 0, "no hook set: memory untouched");

    /* 6. 0.4.378 MODELLED redirect: the instruction at jmp_pc is skipped and execution continues at jmp_to */
    memset(&a, 0, sizeof a);
    a_movz(&a, 0, 1); a_movz(&a, 0, 2); a_movz(&a, 0, 3); a_movz(&a, 1, 7); a_movz(&a, 8, 93); a_svc(&a, 0); a_finish(&a);
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000; c.jmp_pc = 0x1004; c.jmp_to = 0x100c;
    rc = a64_run(&c, 100);
    CHECK(rc == 0 && c.x[0] == 1 && c.x[1] == 7 && c.jmp_n == 1, "redirect skips the instructions between jmp_pc and jmp_to");
    a64_init(&c, mem, 0, sizeof mem); memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, a.w, a.n * 4); c.pc = 0x1000;
    rc = a64_run(&c, 100);
    CHECK(rc == 0 && c.x[0] == 3 && c.jmp_n == 0, "no redirect set: program runs straight through");

    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails != 0;
}
