#include "a64demo.h"
#include "a64asm.h"
#include <string.h>

enum { L_LOOP, L_PRINTNUM, L_PNLOOP, L_FACT, L_REC };

static const char S_HELLO[] = "Hello from my own ARM64 core on Android!\n";
static const char S_SUM[]   = "sum(1..100) = ";
static const char S_FACT[]  = "10! (recursive, bl/ret/stp/ldp) = ";

static void print_str(Asm *a, uint16_t addr, uint16_t len) {
    a_movz(a, 0, 1); a_movz(a, 1, addr); a_movz(a, 2, len); a_movz(a, 8, 64); a_svc(a, 0);
}

void a64_demo_load(A64 *c) {
    static Asm a; memset(&a, 0, sizeof a);

    print_str(&a, 0x2000, sizeof S_HELLO - 1);

    a_movz(&a, 19, 0);                    /* sum = 0 */
    a_movz(&a, 20, 1);                    /* i = 1   */
    a_label(&a, L_LOOP);
    a_add(&a, 19, 19, 20);                /* sum += i */
    a_addi(&a, 20, 20, 1);                /* i++ */
    a_cmpi(&a, 20, 101);
    a_bcond(&a, CC_NE, L_LOOP);

    print_str(&a, 0x2100, sizeof S_SUM - 1);
    a_mov(&a, 0, 19);
    a_bl(&a, L_PRINTNUM);

    print_str(&a, 0x2140, sizeof S_FACT - 1);
    a_movz(&a, 0, 10);
    a_bl(&a, L_FACT);
    a_bl(&a, L_PRINTNUM);

    a_movz(&a, 0, 0);                     /* exit(0) the iOS/BSD way: x16=1, svc #0x80 */
    a_movz(&a, 16, 1);
    a_svc(&a, 0x80);

    /* print_num(x0): decimal + newline via write(1) */
    a_label(&a, L_PRINTNUM);
    a_movz(&a, 9, 0x3020);
    a_movz(&a, 10, 10);
    a_strb(&a, 10, 9, 0);                 /* '\n' (== 10) */
    a_label(&a, L_PNLOOP);
    a_udiv(&a, 11, 0, 10);
    a_msub(&a, 12, 11, 10, 0);            /* r = x0 - q*10 */
    a_addi(&a, 12, 12, 48);
    a_subi(&a, 9, 9, 1);
    a_strb(&a, 12, 9, 0);
    a_mov(&a, 0, 11);
    a_cbnz(&a, 0, L_PNLOOP);
    a_movz(&a, 13, 0x3021);
    a_sub(&a, 2, 13, 9);
    a_mov(&a, 1, 9);
    a_movz(&a, 0, 1);
    a_movz(&a, 8, 64);
    a_svc(&a, 0);
    a_ret(&a);

    /* fact(n): n==0 ? 1 : n * fact(n-1) */
    a_label(&a, L_FACT);
    a_cbnz(&a, 0, L_REC);
    a_movz(&a, 0, 1);
    a_ret(&a);
    a_label(&a, L_REC);
    a_stp_pre(&a, 30, 0, 31, -16);        /* push x30, x0 */
    a_subi(&a, 0, 0, 1);
    a_bl(&a, L_FACT);
    a_ldp_post(&a, 30, 1, 31, 16);        /* pop x30, x1 */
    a_mul(&a, 0, 0, 1);
    a_ret(&a);

    a_finish(&a);

    memcpy(c->mem + 0x1000 - c->base, a.w, (size_t)a.n * 4);
    memcpy(c->mem + 0x2000 - c->base, S_HELLO, sizeof S_HELLO - 1);
    memcpy(c->mem + 0x2100 - c->base, S_SUM, sizeof S_SUM - 1);
    memcpy(c->mem + 0x2140 - c->base, S_FACT, sizeof S_FACT - 1);
    c->pc = 0x1000;
}
