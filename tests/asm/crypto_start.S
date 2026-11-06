.text
.globl _start
_start:
    adr x0, stack_top
    mov sp, x0
    bl cmain
    mov x8, #93
    svc #0
    .p2align 4
    .skip 65536
stack_top:
    .skip 16
