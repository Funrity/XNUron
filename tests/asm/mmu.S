/* System-mode MMU test guest (runs at physical 0x40000000, EL1). Builds 4K-granule, 39-bit page tables itself,
 * enables the MMU, and checks translation, aliasing, page-crossing accesses, translated instruction fetch,
 * fault codes (ESR/FAR), access-flag faults, canonical checks and TLB invalidation.
 * Exit code written to SYSCON: 0 = all passed, N = test N failed. */
.text
.globl _start
.macro CHECKEQ reg, val, n
    ldr x9, =\val
    cmp \reg, x9
    b.eq 1f
    mov x0, #\n
    b fail
1:
.endm
_start:
    adr x1, vectors
    msr vbar_el1, x1
    /* tables */
    ldr x20, =0x40010000        /* L1 for TTBR0 */
    ldr x21, =0x40011000        /* L1 for TTBR1 */
    ldr x22, =0x40012000        /* L2 */
    ldr x23, =0x40013000        /* L3 */
    ldr x1, =0x401              /* block, AF, PA 0 (devices) */
    str x1, [x20, #0]
    ldr x1, =0x40000401         /* block, AF, PA 0x40000000 (identity RAM) */
    str x1, [x20, #8]
    orr x1, x22, #3
    str x1, [x21, #0]
    orr x1, x23, #3
    str x1, [x22, #0]
    ldr x1, =0x40201403         /* L3[0] page0 -> 0x40201000 */
    ldr x2, =0x40201403
    str x2, [x23, #0]
    ldr x2, =0x40205403         /* L3[1] page1 -> 0x40205000 */
    str x2, [x23, #8]
    ldr x2, =0x40202003         /* L3[2] AF = 0 */
    str x2, [x23, #16]
    ldr x2, =0x40201403         /* L3[3] alias of page0 */
    str x2, [x23, #24]
    ldr x2, =0x40206403         /* L3[4] code page */
    str x2, [x23, #32]
    msr ttbr0_el1, x20
    msr ttbr1_el1, x21
    ldr x1, =0x0000000080190019 /* T0SZ=25, T1SZ=25 (<<16), TG1=4K (2<<30) */
    ldr x2, =0x0000000080190019
    ldr x1, =(25 | (25 << 16) | (2 << 30))
    msr tcr_el1, x1
    isb
    mov x1, #1
    msr sctlr_el1, x1
    isb
    ldr x25, =0xFFFFFF8000000000   /* high VA base (TTBR1 region) */
    mov x12, #0

    /* 1. store/load through a high mapping */
    ldr x1, =0x0123456789abcdef
    str x1, [x25, #0x10]
    ldr x2, [x25, #0x10]
    CHECKEQ x2, 0x0123456789abcdef, 1
    /* 2. alias: same physical page through page 3 */
    ldr x2, [x25, #0x3010]
    CHECKEQ x2, 0x0123456789abcdef, 2
    /* 3. physical location through the identity map */
    ldr x3, =0x40201010
    ldr x2, [x3]
    CHECKEQ x2, 0x0123456789abcdef, 3
    /* 4. 8-byte access crossing into a non-contiguous page */
    ldr x1, =0x1122334455667788
    add x4, x25, #0xFFC
    str x1, [x4]
    ldr x3, =0x40201FFC
    ldr w2, [x3]
    CHECKEQ x2, 0x55667788, 4
    ldr x3, =0x40205000
    ldr w2, [x3]
    CHECKEQ x2, 0x11223344, 5
    ldr x2, [x4]
    CHECKEQ x2, 0x1122334455667788, 6
    /* 5. translated instruction fetch from the high mapping */
    ldr x3, =0x40206000
    ldr w1, =0xD2800540         /* mov x0, #42 */
    str w1, [x3]
    ldr w1, =0xD65F03C0         /* ret */
    str w1, [x3, #4]
    ldr x5, =0xFFFFFF8000004000
    blr x5
    CHECKEQ x0, 42, 7
    /* 6. translation fault level 1 on an unmapped TTBR0 address */
    mov x12, #0
    ldr x6, =0x80000000
    ldr x2, [x6]
    cmp x12, #1
    b.eq 2f
    mov x0, #8
    b fail
2:  and x1, x10, #0x3F
    CHECKEQ x1, 0x05, 9
    CHECKEQ x11, 0x80000000, 10
    /* 7. access-flag fault level 3 (HA off) */
    mov x12, #0
    add x6, x25, #0x2000
    ldr x2, [x6]
    cmp x12, #1
    b.eq 3f
    mov x0, #11
    b fail
3:  and x1, x10, #0x3F
    CHECKEQ x1, 0x0B, 12
    /* 8. non-canonical address -> translation fault level 0, store sets WnR */
    mov x12, #0
    ldr x6, =0x0001000000000000
    str x2, [x6]
    and x1, x10, #0x3F
    CHECKEQ x1, 0x04, 13
    tst x10, #0x40
    b.ne 4f
    mov x0, #14
    b fail
4:
    /* 9. TLB invalidation: remap page 0, invalidate, see the new page */
    ldr x3, =0x40207010
    ldr x1, =0xABCDEF0011223344
    str x1, [x3]
    ldr x1, =0x40207403
    str x1, [x23, #0]
    tlbi vmalle1
    dsb ish
    isb
    ldr x2, [x25, #0x10]
    CHECKEQ x2, 0xABCDEF0011223344, 15
    /* 10. AT S1E1R: translation into PAR_EL1 (what the kernel's kvtophys uses) */
    at s1e1r, x25
    mrs x4, par_el1
    and x5, x4, #1
    CHECKEQ x5, 0, 16
    and x5, x4, #0x0000FFFFFFFFF000
    CHECKEQ x5, 0x40207000, 17
    ldr x6, =0x0001000000000000
    at s1e1r, x6
    mrs x4, par_el1
    and x5, x4, #1
    CHECKEQ x5, 1, 18
    /* DC ZVA zeroes the whole 64-byte block that contains the address (the kernel's bzero relies on it) */
    ldr x7, =0x40300000
    mov x8, #-1
    mov x11, #0
zfill:
    str x8, [x7, x11]
    add x11, x11, #8
    cmp x11, #256
    b.lo zfill
    add x10, x7, #0x50
    dc zva, x10
    ldr x2, [x7, #0x38]
    CHECKEQ x2, 0xFFFFFFFFFFFFFFFF, 19
    ldr x2, [x7, #0x40]
    CHECKEQ x2, 0, 20
    ldr x2, [x7, #0x78]
    CHECKEQ x2, 0, 21
    ldr x2, [x7, #0x80]
    CHECKEQ x2, 0xFFFFFFFFFFFFFFFF, 22
    mrs x2, dczid_el0
    CHECKEQ x2, 4, 23
    /* 11. write permission (0.4.394): TTBR0 L1[3] = read-only block (AP[2]=1) onto RAM; a load works, a store gives permission fault level 1 (0x0D) with WnR and leaves memory unchanged */
    ldr x1, =0x40000481
    str x1, [x20, #24]
    tlbi vmalle1
    dsb ish
    isb
    ldr x7, =0xC0310000
    ldr x2, [x7]
    mov x12, #0
    ldr x3, =0x5555555555555555
    str x3, [x7]
    cmp x12, #1
    b.eq 5f
    mov x0, #24
    b fail
5:  and x1, x10, #0x3F
    CHECKEQ x1, 0x0D, 25
    tst x10, #0x40
    b.ne 6f
    mov x0, #26
    b fail
6:  ldr x8, =0x40310000
    ldr x2, [x8]
    cmp x2, x3
    b.ne 7f
    mov x0, #27
    b fail
7:
    /* done */
    mov x0, #0
fail:
    ldr x1, =0x0A002000
    str w0, [x1]
    b .

.balign 0x800
vectors:
    .rept 0x200/4
    b .
    .endr
    /* current EL, SPx, synchronous: record and skip the faulting instruction */
    mrs x10, esr_el1
    mrs x11, far_el1
    add x12, x12, #1
    mrs x13, elr_el1
    add x13, x13, #4
    msr elr_el1, x13
    eret
