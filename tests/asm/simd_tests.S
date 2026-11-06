/* Hand-written SIMD/FP checks with expected values worked out by hand (user mode, exit code = failing check number, 0 = ok). */
.text
.globl _start
.macro EXPECT reg, val, n
    ldr x9, =\val
    cmp \reg, x9
    b.eq 1f
    mov x0, #\n
    b done
1:
.endm
_start:
    /* MOVI / MVNI / ORR / DUP / UMOV */
    movi v0.16b, #0xAB
    umov x1, v0.d[1]
    EXPECT x1, 0xABABABABABABABAB, 1
    movi v1.4s, #0x12, lsl #8
    umov w2, v1.s[3]
    EXPECT x2, 0x1200, 2
    mvni v2.4s, #0x0F
    umov w2, v2.s[0]
    EXPECT x2, 0xFFFFFFF0, 3
    movi v3.2d, #0xFF00FF00FF00FF00
    umov x1, v3.d[0]
    EXPECT x1, 0xFF00FF00FF00FF00, 4
    ldr x5, =0x1122334455667788
    dup v4.2d, x5
    umov x1, v4.d[1]
    EXPECT x1, 0x1122334455667788, 5
    /* lane moves and arithmetic */
    ins v4.s[1], w5
    umov w1, v4.s[1]
    EXPECT x1, 0x55667788, 6
    movi v5.4s, #3
    movi v6.4s, #4
    add v7.4s, v5.4s, v6.4s
    umov w1, v7.s[2]
    EXPECT x1, 7, 7
    mul v7.4s, v5.4s, v6.4s
    umov w1, v7.s[0]
    EXPECT x1, 12, 8
    sub v7.4s, v5.4s, v6.4s
    smov x1, v7.s[1]
    EXPECT x1, 0xFFFFFFFFFFFFFFFF, 9
    addv s8, v6.4s
    fmov w1, s8
    EXPECT x1, 16, 10
    cnt v9.16b, v0.16b
    umov x1, v9.d[0]
    EXPECT x1, 0x0505050505050505, 11
    /* ld1/st1/ldp/stp q + structure loads */
    sub sp, sp, #64
    stp q0, q3, [sp]
    ldp q10, q11, [sp]
    umov x1, v11.d[0]
    EXPECT x1, 0xFF00FF00FF00FF00, 12
    mov w6, #1
    mov w7, #2
    stp w6, w7, [sp]
    stp w7, w6, [sp, #8]
    ld2 {v12.2s, v13.2s}, [sp]
    umov w1, v12.s[1]
    EXPECT x1, 2, 13
    umov w1, v13.s[0]
    EXPECT x1, 2, 14
    add sp, sp, #64
    /* scalar FP */
    fmov d0, #2.0
    fmov d1, #0.5
    fmul d2, d0, d1
    fmov d3, #1.0
    fcmp d2, d3
    b.ne bad
    fadd d4, d0, d1
    fcvtzs x1, d4
    EXPECT x1, 2, 15
    fmov d5, #4.0
    fsqrt d6, d5
    fcmp d6, d0
    b.ne bad
    mov x2, #-7
    scvtf d7, x2
    fneg d8, d7
    fcvtzs x1, d8
    EXPECT x1, 7, 16
    fcvt s9, d0
    fmov w1, s9
    EXPECT x1, 0x40000000, 17
    fmadd d10, d0, d0, d1                  /* 0.5 + 2*2 = 4.5 */
    fmov x1, d10
    EXPECT x1, 0x4012000000000000, 18
    fcsel d11, d0, d1, ne
    fmov x1, d11
    EXPECT x1, 0x3FE0000000000000, 19      /* flags still "equal" from the last fcmp: ne is false -> d1 (0.5) */
    /* halving / saturating byte arithmetic */
    movi v20.16b, #200
    movi v21.16b, #100
    uhadd v22.16b, v20.16b, v21.16b
    umov w1, v22.b[5]
    EXPECT x1, 150, 20
    uqadd v22.16b, v20.16b, v21.16b
    umov w1, v22.b[5]
    EXPECT x1, 255, 21
    sqadd v22.16b, v20.16b, v21.16b
    umov w1, v22.b[5]
    EXPECT x1, 44, 22                      /* -56 + 100 */
    uhsub v22.16b, v21.16b, v20.16b
    umov w1, v22.b[5]
    EXPECT x1, 206, 23                     /* (100-200)>>1 = -50 */
    srhadd v22.16b, v20.16b, v21.16b
    umov w1, v22.b[5]
    EXPECT x1, 22, 24                      /* (-56+100+1)>>1 */
    movi v23.16b, #127
    sqadd v22.16b, v23.16b, v23.16b
    umov w1, v22.b[5]
    EXPECT x1, 127, 25
    mov x0, #0
    b done
bad:
    mov x0, #99
done:
    mov x8, #93
    svc #0
