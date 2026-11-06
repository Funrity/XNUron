/* Kernel that installs a spinning vector table, then touches an unknown non-Apple register: the report must name the cause. */
.text
.globl _start
_start:
    adr x1, vectors
    msr vbar_el1, x1
    mrs x2, S3_5_C9_C9_5        /* not in the Apple C15 space: genuinely unimplemented */
    b .
.balign 0x800
vectors:
    .rept 0x800/4
    b .
    .endr
