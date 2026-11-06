// XNUron test guest: bare-metal AArch64 kernel for the a64virt machine.
// This is our own test software. It is NOT iOS, XNU or any Apple code.
    .section .text.boot, "ax"
    .globl _start
_start:
    mov     x19, x0                  // boot flags (a64virt: bit0 = stay interactive)
    adrp    x1, stack_top
    add     x1, x1, :lo12:stack_top
    mov     sp, x1
    adrp    x1, vectors
    add     x1, x1, :lo12:vectors
    msr     vbar_el1, x1
    isb
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:  mov     x0, x19
    bl      kmain
3:  wfi
    b       3b

    .globl enter_el0
enter_el0:                           // x0 = entry, x1 = EL0 stack
    stp     x19, x20, [sp, #-96]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x2, sp
    adrp    x3, saved_ksp
    str     x2, [x3, :lo12:saved_ksp]
    msr     sp_el0, x1
    msr     elr_el1, x0
    msr     spsr_el1, xzr            // EL0t, interrupts enabled
    eret

    .globl el0_return
el0_return:
    adrp    x3, saved_ksp
    ldr     x2, [x3, :lo12:saved_ksp]
    mov     sp, x2
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     x19, x20, [sp], #96
    ret

.macro VECTOR n
    .balign 0x80
    stp     x29, x30, [sp, #-16]!
    mov     x29, #\n
    b       common_entry
.endm
    .balign 0x800
    .globl vectors
vectors:
    VECTOR 0
    VECTOR 1
    VECTOR 2
    VECTOR 3
    VECTOR 4
    VECTOR 5
    VECTOR 6
    VECTOR 7
    VECTOR 8
    VECTOR 9
    VECTOR 10
    VECTOR 11
    VECTOR 12
    VECTOR 13
    VECTOR 14
    VECTOR 15

common_entry:                        // x29 = vector number, x29/x30 already pushed
    sub     sp, sp, #(16 * 15)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    str     x28, [sp, #224]
    mov     x1, x29                  // vector number
    mov     x0, sp                   // frame (x0..x28)
    bl      exc_handler
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldr     x28, [sp, #224]
    add     sp, sp, #(16 * 15)
    ldp     x29, x30, [sp], #16
    eret

    .section .bss
    .balign 16
    .skip 0x10000
stack_top:
    .balign 8
saved_ksp: .skip 8
