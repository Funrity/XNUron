// Hand-written load/store/branch tests with hand-computed expectations.
.macro CHECK reg, val, id
    movz x28, #(\val) & 0xFFFF
    movk x28, #((\val) >> 16) & 0xFFFF, lsl #16
    movk x28, #((\val) >> 32) & 0xFFFF, lsl #32
    movk x28, #((\val) >> 48) & 0xFFFF, lsl #48
    cmp \reg, x28
    b.ne 99f
.endm
.macro NEXT id
    add x27, x27, #1
.endm
.text
.globl _start
_start:
    mov x27, #0
    adr x20, buf
    // data: bytes 0x80,0x81,...  -> buf[i] = 0x80+i
    // ---- sign/zero extension loads
    ldrsb x0, [x20]            // 0x80 -> 0xFFFFFFFFFFFFFF80
    CHECK x0, 0xFFFFFFFFFFFFFF80, 1
    ldrsb w0, [x20]            // -> 0x00000000FFFFFF80
    CHECK x0, 0xFFFFFF80, 2
    ldrb w0, [x20, #1]
    CHECK x0, 0x81, 3
    ldrsh x0, [x20, #2]        // bytes 82 83 -> 0x8382
    CHECK x0, 0xFFFFFFFFFFFF8382, 4
    ldrsh w0, [x20, #2]
    CHECK x0, 0xFFFF8382, 5
    ldrsw x0, [x20, #4]        // 84 85 86 87 -> 0x87868584
    CHECK x0, 0xFFFFFFFF87868584, 6
    ldr w0, [x20, #4]
    CHECK x0, 0x87868584, 7
    ldr x0, [x20, #8]
    CHECK x0, 0x8F8E8D8C8B8A8988, 8
    ldur x0, [x20, #1]         // unaligned unscaled
    CHECK x0, 0x8887868584838281, 9
    // ---- register offset with extends
    mov x1, #2
    ldrb w0, [x20, x1]
    CHECK x0, 0x82, 10
    ldrh w0, [x20, x1, lsl #1] // offset 4 -> 84 85
    CHECK x0, 0x8584, 11
    mov x1, #-2
    add x21, x20, #8
    ldrb w0, [x21, w1, sxtw]   // buf[6]
    CHECK x0, 0x86, 12
    // ---- pre/post index
    mov x22, x20
    ldrb w0, [x22], #3         // loads buf[0], x22 = buf+3
    CHECK x0, 0x80, 13
    sub x1, x22, x20
    CHECK x1, 3, 14
    ldrb w0, [x22, #2]!        // x22 = buf+5, loads buf[5]
    CHECK x0, 0x85, 15
    sub x1, x22, x20
    CHECK x1, 5, 16
    // ---- store + load back, pre/post index
    adr x23, scratch
    mov x1, #0x1234
    strh w1, [x23], #2
    mov x1, #0xABCD
    strh w1, [x23], #2
    ldr w0, [x23, #-4]
    CHECK x0, 0xABCD1234, 17
    str xzr, [x23, #-4]!
    ldr x0, [x23]
    CHECK x0, 0, 18
    // ---- pairs
    mov x1, #0x1111
    mov x2, #0x2222
    stp x1, x2, [x23, #-16]!
    ldp x3, x4, [x23], #16
    CHECK x3, 0x1111, 19
    CHECK x4, 0x2222, 20
    sub x1, x23, #16
    adr x5, scratch
    sub x5, x5, #4
    // ldpsw: two 32-bit words sign-extended
    adr x6, words
    ldpsw x7, x8, [x6]
    CHECK x7, 0xFFFFFFFF80000001, 21
    CHECK x8, 0x7FFFFFFF, 22
    ldp w9, w10, [x6, #8]
    CHECK x9, 0x11223344, 23
    CHECK x10, 0x55667788, 24
    // ---- literal loads
    ldr x0, lit64
    CHECK x0, 0x0123456789ABCDEF, 25
    ldr w0, lit64
    CHECK x0, 0x89ABCDEF, 26
    ldrsw x0, lit32n
    CHECK x0, 0xFFFFFFFFFFFFFFFE, 27
    // ---- adrp/adr consistency
    adrp x0, scratch
    add x0, x0, :lo12:scratch
    adr x1, scratch
    cmp x0, x1
    b.ne 99f
    // ---- test-and-branch
    movz x0, #0x10
    movk x0, #0x8000, lsl #48
    tbz x0, #4, 99f
    tbnz x0, #3, 99f
    tbnz x0, #63, 2f
    b 99f
2:  tbz x0, #63, 99f
    // ---- exclusive monitor: stxr fails without ldxr, succeeds after
    adr x24, scratch
    mov x1, #77
    stxr w2, x1, [x24]
    CHECK x2, 1, 28
    ldxr x3, [x24]
    stxr w2, x1, [x24]
    CHECK x2, 0, 29
    ldr x0, [x24]
    CHECK x0, 77, 30
    stxr w2, x1, [x24]         // monitor cleared by previous stxr
    CHECK x2, 1, 31
    stlr x1, [x24]
    ldar x0, [x24]
    CHECK x0, 77, 32
    // ---- cbz / cbnz with 32-bit view
    mov x0, #0x100000000
    cbz w0, 3f
    b 99f
3:  cbnz x0, 4f
    b 99f
4:  // ---- bl / ret / br / blr
    adr x9, fn
    blr x9
    CHECK x0, 1234, 33
    adr x9, 5f
    br x9
    b 99f
5:  mov x0, #0
    b exit
99: mov x0, x27
    add x0, x0, #1000
exit:
    mov x8, #93
    svc #0
fn:
    mov x0, #1234
    ret
    .p2align 3
lit64: .quad 0x0123456789ABCDEF
lit32n: .word 0xFFFFFFFE
    .p2align 3
words: .word 0x80000001, 0x7FFFFFFF, 0x11223344, 0x55667788
buf:
    .byte 0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F
    .p2align 3
scratch: .skip 64
