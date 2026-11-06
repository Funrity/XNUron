// Own test program in the XNU/arm64 user ABI (BSD syscalls via svc #0x80). Not Apple code.
.globl _start
.text
.p2align 2
_start:
    mov x0, #1
    adrp x1, msg@PAGE
    add x1, x1, msg@PAGEOFF
    mov x2, #37
    mov x16, #4          // SYS_write
    svc #0x80
    mov x16, #20         // SYS_getpid
    svc #0x80
    mov x0, x0
    mov x16, #999        // not implemented -> ENOSYS (carry set)
    svc #0x80
    mov x0, #7
    mov x16, #1          // SYS_exit
    svc #0x80
.section __TEXT,__const
msg: .ascii "hello from a Mach-O arm64 (own code)\n"
