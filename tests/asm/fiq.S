/* MODELLED CNTP FIQ test (system mode, EL1, physical 0x40000000).
 * 1: with F masked and the timer expired, no FIQ is taken (the loop reaches its counter).
 * 2: after unmasking F the FIQ arrives at VBAR+0x300 (current EL, SPx, FIQ) with ELR inside the loop.
 * 3: WFI wakes up on the physical timer.  Exit code 0 = all passed, N = test N failed. */
.text
.globl _start
_start:
    ldr x1, =0x0A002000          /* SYSCON */
    adr x2, vectors
    msr vbar_el1, x2
    mov x1, #1
    msr spsel, x1
    ldr x20, =0x40100000         /* FIQ counter */
    str xzr, [x20]
    msr daifset, #1              /* mask F */
    mrs x3, cntpct_el0
    add x3, x3, #50
    msr cntp_cval_el0, x3
    mov x3, #1
    msr cntp_ctl_el0, x3         /* enable, unmasked */
    mov x4, #200
1:  subs x4, x4, #1
    b.ne 1b
    ldr x5, [x20]
    mov x0, #1
    cbnz x5, fail                /* FIQ taken while masked */
    msr daifclr, #1              /* unmask F: FIQ now */
    nop
    nop
    ldr x5, [x20]
    mov x0, #2
    cmp x5, #1
    b.ne fail
    /* handler disabled the timer; arm it again and sleep with WFI */
    mrs x3, cntpct_el0
    ldr x9, =100000
    add x3, x3, x9
    msr cntp_cval_el0, x3
    mov x3, #1
    msr cntp_ctl_el0, x3
    wfi
    ldr x5, [x20]
    mov x0, #3
    cmp x5, #2
    b.ne fail
    mov x0, #0
fail:
    ldr x1, =0x0A002000
    str w0, [x1]
    b .
.balign 0x800
vectors:
    .rept 0x300/4
    b .
    .endr
    /* +0x300: FIQ from current EL with SPx */
    mov x6, #2
    msr cntp_ctl_el0, x6         /* mask + disable: stops the interrupt */
    ldr x7, =0x40100000
    ldr x8, [x7]
    add x8, x8, #1
    str x8, [x7]
    eret
