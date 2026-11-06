/* Test "kernel" for the IPSW attempt: prints, checks boot_args, prints the DeviceTree blob, then enables the MMU. */
.text
.globl _start
_start:
    mov x20, x0
    movz x2, #0x900, lsl #16
    adr x1, msg1
    bl puts
    ldrh w4, [x20]              /* boot_args.Revision must be 2 */
    cmp w4, #2
    b.ne bad
    ldr x7, [x20, #96]          /* deviceTreeP */
    cbz x7, nodt
    ldr x5, [x20, #8]           /* deviceTreeP is a kernel virtual address: convert with virtBase/physBase */
    ldr x6, [x20, #16]
    sub x7, x7, x5
    add x7, x7, x6
    mov x1, x7
    bl puts
nodt:
    mov x8, #0x55
    msr S3_4_C15_C2_1, x8       /* Apple implementation-defined register: stubbed, must be logged */
    mrs x9, S3_0_C15_C1_0
    mrs x11, S3_4_C15_C0_4      /* modelled "ready" bit: must read 2 */
    cmp x11, #2
    b.ne bad
    msr S3_5_C15_C3_3, x8       /* read-back model */
    mrs x12, S3_5_C15_C3_3
    cmp x12, x8
    b.ne bad
    msr S3_0_C2_C1_2, x8        /* APIBKeyLo_EL1: PAC key registers must exist */
    msr S3_0_C2_C2_3, x8        /* APDBKeyHi_EL1 */
    mrs x13, S3_0_C2_C1_2
    cmp x13, x8
    b.ne bad
    msr tpidrro_el0, x8         /* architectural EL1-writable register */
    mrs x10, tpidrro_el0
    cmp x10, x8
    b.ne bad
    .inst 0xd500419f            /* msr pan, #1 (ARMv8.1): tracked */
    mrs x13, S3_0_C4_C2_3
    mov x14, #0x400000
    cmp x13, x14
    b.ne bad
    .inst 0xd500409f            /* msr pan, #0 */
    mrs x13, S3_0_C4_C2_3
    cbnz x13, bad
    adr x1, msg2
    bl puts
    mov x6, #1
    msr sctlr_el1, x6           /* MMU on with no tables: next fetch must fault and be reported */
    b .
bad:
    movz x2, #0xA00, lsl #16
    movk x2, #0x2000, lsl #0
    mov w3, #7
    str w3, [x2]
    b .
puts:
    ldrb w3, [x1], #1
    cbz w3, 1f
    strb w3, [x2]
    b puts
1:  ret
msg1: .asciz "test-kernel: started at EL1\n"
msg2: .asciz "\ntest-kernel: enabling MMU now\n"
