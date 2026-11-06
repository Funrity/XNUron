/* Tiny AArch64 assembler (just the instructions our core understands). */
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t w[512]; int n;
    int lbl[32];
    struct { int idx, label, kind; } fx[64]; int nfx;
} Asm;

static inline void a_e(Asm *a, uint32_t w) { a->w[a->n++] = w; }
static inline void a_label(Asm *a, int id) { a->lbl[id] = a->n; }
static inline void a_fix(Asm *a, uint32_t w, int label, int kind) {
    a->fx[a->nfx].idx = a->n; a->fx[a->nfx].label = label; a->fx[a->nfx].kind = kind; a->nfx++;
    a_e(a, w);
}
static inline void a_movz(Asm *a, int rd, uint16_t imm) { a_e(a, 0xD2800000u | (uint32_t)imm << 5 | rd); }
static inline void a_movk(Asm *a, int rd, uint16_t imm, int hw) { a_e(a, 0xF2800000u | hw << 21 | (uint32_t)imm << 5 | rd); }
static inline void a_addi(Asm *a, int rd, int rn, int imm) { a_e(a, 0x91000000u | imm << 10 | rn << 5 | rd); }
static inline void a_subi(Asm *a, int rd, int rn, int imm) { a_e(a, 0xD1000000u | imm << 10 | rn << 5 | rd); }
static inline void a_cmpi(Asm *a, int rn, int imm)         { a_e(a, 0xF1000000u | imm << 10 | rn << 5 | 31); }
static inline void a_add(Asm *a, int rd, int rn, int rm)   { a_e(a, 0x8B000000u | rm << 16 | rn << 5 | rd); }
static inline void a_sub(Asm *a, int rd, int rn, int rm)   { a_e(a, 0xCB000000u | rm << 16 | rn << 5 | rd); }
static inline void a_mov(Asm *a, int rd, int rm)           { a_e(a, 0xAA0003E0u | rm << 16 | rd); }
static inline void a_mul(Asm *a, int rd, int rn, int rm)   { a_e(a, 0x9B007C00u | rm << 16 | rn << 5 | rd); }
static inline void a_udiv(Asm *a, int rd, int rn, int rm)  { a_e(a, 0x9AC00800u | rm << 16 | rn << 5 | rd); }
static inline void a_msub(Asm *a, int rd, int rn, int rm, int ra) { a_e(a, 0x9B008000u | rm << 16 | ra << 10 | rn << 5 | rd); }
static inline void a_strb(Asm *a, int rt, int rn, int imm) { a_e(a, 0x39000000u | imm << 10 | rn << 5 | rt); }
static inline void a_ldrb(Asm *a, int rt, int rn, int imm) { a_e(a, 0x39400000u | imm << 10 | rn << 5 | rt); }
static inline void a_str(Asm *a, int rt, int rn, int imm)  { a_e(a, 0xF9000000u | (imm / 8) << 10 | rn << 5 | rt); }
static inline void a_ldr(Asm *a, int rt, int rn, int imm)  { a_e(a, 0xF9400000u | (imm / 8) << 10 | rn << 5 | rt); }
static inline void a_stp_pre(Asm *a, int rt, int rt2, int rn, int imm)  { a_e(a, 0xA9800000u | (uint32_t)((imm / 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt); }
static inline void a_ldp_post(Asm *a, int rt, int rt2, int rn, int imm) { a_e(a, 0xA8C00000u | (uint32_t)((imm / 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt); }
static inline void a_svc(Asm *a, int imm) { a_e(a, 0xD4000001u | (uint32_t)imm << 5); }
static inline void a_ret(Asm *a) { a_e(a, 0xD65F03C0u); }
static inline void a_b(Asm *a, int l)  { a_fix(a, 0x14000000u, l, 0); }
static inline void a_bl(Asm *a, int l) { a_fix(a, 0x94000000u, l, 0); }
static inline void a_bcond(Asm *a, int cc, int l) { a_fix(a, 0x54000000u | cc, l, 1); }
static inline void a_cbz(Asm *a, int rt, int l)  { a_fix(a, 0xB4000000u | rt, l, 1); }
static inline void a_cbnz(Asm *a, int rt, int l) { a_fix(a, 0xB5000000u | rt, l, 1); }
enum { CC_EQ = 0, CC_NE = 1, CC_LT = 11, CC_GE = 10, CC_GT = 12, CC_LE = 13 };

static inline void a_finish(Asm *a) {
    for (int k = 0; k < a->nfx; k++) {
        int d = a->lbl[a->fx[k].label] - a->fx[k].idx;
        if (a->fx[k].kind == 0) a->w[a->fx[k].idx] |= (uint32_t)d & 0x3FFFFFFu;
        else a->w[a->fx[k].idx] |= ((uint32_t)d & 0x7FFFFu) << 5;
    }
}
