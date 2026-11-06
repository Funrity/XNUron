/* Test "kernel" that waits forever for a value that never appears (like a poll of absent hardware). */
.text
.globl _start
_start:
    mov x20, x0
    movz x2, #0x900, lsl #16
    mov w3, #'s'
    strb w3, [x2]
1:  ldr x1, [x20, #0x100]
    cbz x1, 1b
    b .
