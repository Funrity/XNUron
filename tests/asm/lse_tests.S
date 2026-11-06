// ARMv8.1 LSE atomics (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP/CAS/CASP) and LDXP/STXP, hand-computed expectations.
.arch_extension lse
.macro CHECK reg, val
    movz x28, #(\val) & 0xFFFF
    movk x28, #((\val) >> 16) & 0xFFFF, lsl #16
    movk x28, #((\val) >> 32) & 0xFFFF, lsl #32
    movk x28, #((\val) >> 48) & 0xFFFF, lsl #48
    cmp \reg, x28
    b.ne 99f
.endm
.macro NEXT
    add x27, x27, #1
.endm
.macro SETM val
    movz w9, #(\val) & 0xFFFF
    movk w9, #((\val) >> 16) & 0xFFFF, lsl #16
    str w9, [x20]
.endm
.text
.globl _start
_start:
    mov x27, #0
    adr x20, mem
    // ldadd: mem=10, +5 -> old 10, mem 15
    SETM 10
    mov w1, #5
    ldadd w1, w2, [x20]
    CHECK x2, 10
    ldr w3, [x20]
    CHECK x3, 15
    NEXT
    // stadd (Rt = wzr): mem += 5 -> 20, acquire/release forms too
    stadd w1, [x20]
    ldaddal w1, w2, [x20]
    CHECK x2, 20
    ldr w3, [x20]
    CHECK x3, 25
    NEXT
    // ldclr 0xFF & ~0x0F = 0xF0
    SETM 0xFF
    mov w1, #0x0F
    ldclr w1, w2, [x20]
    CHECK x2, 0xFF
    ldr w3, [x20]
    CHECK x3, 0xF0
    NEXT
    // ldeor 0xFF ^ 0x0F = 0xF0 ; ldset 0xF0 | 0x0F = 0xFF
    SETM 0xFF
    ldeor w1, w2, [x20]
    ldr w3, [x20]
    CHECK x3, 0xF0
    ldset w1, w2, [x20]
    CHECK x2, 0xF0
    ldr w3, [x20]
    CHECK x3, 0xFF
    NEXT
    // signed / unsigned max / min on 32 bit: mem = -5, operand 3
    mov w1, #3
    SETM 0xFFFFFFFB
    ldsmax w1, w2, [x20]
    CHECK x2, 0xFFFFFFFB
    ldr w3, [x20]
    CHECK x3, 3
    SETM 0xFFFFFFFB
    ldumax w1, w2, [x20]
    ldr w3, [x20]
    CHECK x3, 0xFFFFFFFB
    SETM 0xFFFFFFFB
    ldsmin w1, w2, [x20]
    ldr w3, [x20]
    CHECK x3, 0xFFFFFFFB
    SETM 0xFFFFFFFB
    ldumin w1, w2, [x20]
    ldr w3, [x20]
    CHECK x3, 3
    NEXT
    // swp
    SETM 7
    mov w1, #9
    swp w1, w2, [x20]
    CHECK x2, 7
    ldr w3, [x20]
    CHECK x3, 9
    NEXT
    // byte / halfword wrap-around
    SETM 0x1234FFFF
    mov w1, #1
    ldaddb w1, w2, [x20]
    CHECK x2, 0xFF
    ldr w3, [x20]
    CHECK x3, 0x1234FF00
    ldaddh w1, w2, [x20]
    CHECK x2, 0xFF00
    ldr w3, [x20]
    CHECK x3, 0x1234FF01
    NEXT
    // 64-bit ldadd
    movz x9, #0xFFFF
    movk x9, #0xFFFF, lsl #16
    str x9, [x20]
    mov x1, #1
    ldadd x1, x2, [x20]
    CHECK x2, 0xFFFFFFFF
    ldr x3, [x20]
    CHECK x3, 0x100000000
    NEXT
    // cas success: mem=5 expected 5 new 9
    SETM 5
    mov w1, #5
    mov w2, #9
    cas w1, w2, [x20]
    CHECK x1, 5
    ldr w3, [x20]
    CHECK x3, 9
    NEXT
    // cas failure: expected 6, mem=9: x1 gets old 9, memory unchanged
    mov w1, #6
    mov w2, #77
    casal w1, w2, [x20]
    CHECK x1, 9
    ldr w3, [x20]
    CHECK x3, 9
    NEXT
    // casp 64-bit pair: mem = {1,2}, expected {1,2}, new {7,8}
    mov x9, #1
    mov x10, #2
    mov x2, #1
    mov x3, #2
    mov x4, #7
    mov x5, #8
    adr x21, pairm
    stp x9, x10, [x21]
    casp x2, x3, x4, x5, [x21]
    ldp x6, x7, [x21]
    CHECK x6, 7
    CHECK x7, 8
    CHECK x2, 1
    CHECK x3, 2
    NEXT
    // ldxp / stxp
    ldxp x6, x7, [x21]
    CHECK x6, 7
    CHECK x7, 8
    mov x6, #70
    mov x7, #80
    stxp w8, x6, x7, [x21]
    CHECK x8, 0
    ldp x9, x10, [x21]
    CHECK x9, 70
    CHECK x10, 80
    NEXT
    mov x0, #0
    b exit
99: mov x0, x27
    add x0, x0, #1000
exit:
    mov x8, #93
    svc #0
    .p2align 4
mem: .skip 16
pairm: .skip 16
