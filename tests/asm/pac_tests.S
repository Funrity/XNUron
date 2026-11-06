// Pointer-authentication instructions: model = signatures are not computed, so PAC*/AUT* leave pointers unchanged,
// XPAC* canonicalises, PACGA is a deterministic MAC in the top 32 bits, and the PAC branch forms behave like plain branches.
.arch armv8.3-a
.text
.globl _start
_start:
    mov x27, #0
    adr x20, target
    // PACIA/AUTIA keep the pointer
    mov x0, x20
    mov x1, #0x1234
    pacia x0, x1
    cmp x0, x20
    b.ne 99f
    add x27, x27, #1
    autia x0, x1
    cmp x0, x20
    b.ne 99f
    add x27, x27, #1
    // zero-modifier forms
    mov x0, x20
    paciza x0
    autiza x0
    pacdb x0, x1
    autdb x0, x1
    cmp x0, x20
    b.ne 99f
    add x27, x27, #1
    // XPACI on a kernel-style pointer (bit 55 set) restores the upper byte
    movz x0, #0xFFF0, lsl #48
    movk x0, #0x1234
    movk x0, #0x00FF, lsl #32
    orr x0, x0, #(1 << 55)
    and x0, x0, #0x00FFFFFFFFFFFFFF
    orr x0, x0, #(1 << 55)
    xpaci x0
    lsr x2, x0, #56
    cmp x2, #0xFF
    b.ne 99f
    add x27, x27, #1
    // PACGA: low 32 bits zero, deterministic
    mov x0, #0x1111
    mov x1, #0x2222
    pacga x2, x0, x1
    pacga x3, x0, x1
    cmp x2, x3
    b.ne 99f
    mov w4, w2
    cbnz w4, 99f
    add x27, x27, #1
    // BRAAZ / BLRAAZ / BRAA: plain jumps in this model
    adr x0, hit1
    braaz x0
    b 99f
hit1:
    add x27, x27, #1
    adr x0, fn
    blraaz x0
    add x27, x27, #1
    adr x0, hit2
    mov x1, #7
    braa x0, x1
    b 99f
hit2:
    add x27, x27, #1
    // PACIASP / RETAA pair (prologue/epilogue as emitted by arm64e compilers)
    bl pfunc
    cmp x0, #77
    b.ne 99f
    add x27, x27, #1
    // LDRAA with offset and pre-index
    adr x5, tbl
    ldraa x6, [x5, #8]
    cmp x6, #0x22
    b.ne 99f
    ldraa x6, [x5, #16]!
    cmp x6, #0x33
    b.ne 99f
    adr x7, tbl
    add x7, x7, #16
    cmp x5, x7
    b.ne 99f
    add x27, x27, #1
    cmp x27, #10
    b.ne 99f
    mov x0, #0
    b exit
99: mov x0, x27
    add x0, x0, #1000
exit:
    mov x8, #93
    svc #0
fn:
    ret
pfunc:
    paciasp
    mov x0, #77
    retaa
target: nop
    .p2align 3
tbl: .quad 0x11, 0x22, 0x33
