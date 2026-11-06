#!/usr/bin/env python3
"""Generates tests/asm/insn_tests.S: per-instruction tests whose expected values are
computed here by an independent Python model of the ARM ARM pseudocode."""
import random, sys
random.seed(64)
M64, M32 = (1 << 64) - 1, (1 << 32) - 1
def mask(sf): return M64 if sf else M32
def sgn(v, w): v &= (1 << w) - 1; return v - (1 << w) if v >> (w - 1) else v
def ror(v, r, w): v &= (1 << w) - 1; r %= w; return ((v >> r) | (v << (w - r))) & ((1 << w) - 1)
def addc(a, b, c, w):
    m = (1 << w) - 1; a &= m; b &= m
    u = a + b + c; r = u & m
    s = sgn(a, w) + sgn(b, w) + c
    return r, (r >> (w - 1)) & 1, int(r == 0), int(u > m), int(s != sgn(r, w))
def nzcv(n, z, c, v): return (n << 31) | (z << 30) | (c << 29) | (v << 28)
EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
        0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, M64, 0x00FF00FF00FF00FF, 0x123456789ABCDEF0,
        0xDEADBEEFCAFEBABE]
def vals(n=6):
    return [random.choice(EDGE) if random.random() < .6 else random.getrandbits(64) for _ in range(n)]

cases = []   # (asm lines, expected x0, expected nzcv or None)
def add(lines, x0, fl=None, init_fl=None):
    cases.append((lines, x0 & M64, fl, init_fl))
def regs(a, b, extra=""):
    return [f"mov x1, #{a}", f"mov x2, #{b}"] + ([extra] if extra else [])

CONDS = ["eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al"]
def cond_true(cc, n, z, c, v):
    return {"eq": z, "ne": not z, "cs": c, "cc": not c, "mi": n, "pl": not n, "vs": v, "vc": not v,
            "hi": c and not z, "ls": not (c and not z), "ge": n == v, "lt": n != v,
            "gt": (not z) and n == v, "le": z or n != v, "al": True}[cc]

for a, b in zip(vals(10), vals(10)):
    for sf, r in ((1, "x"), (0, "w")):
        w = 64 if sf else 32; m = mask(sf)
        # flag-setting arithmetic
        for mn, f in (("adds", lambda a, b: addc(a, b, 0, w)), ("subs", lambda a, b: addc(a, ~b, 1, w))):
            res, n, z, c, v = f(a & m, b & m)
            add(regs(a, b) + [f"{mn} {r}0, {r}1, {r}2"], res, nzcv(n, z, c, v))
        # carry in
        for cin in (0, 1):
            ini = nzcv(0, 0, cin, 0)
            res, n, z, c, v = addc(a, b, cin, w); add(regs(a, b) + [f"adcs {r}0, {r}1, {r}2"], res, nzcv(n, z, c, v), ini)
            res, n, z, c, v = addc(a, ~b, cin, w); add(regs(a, b) + [f"sbcs {r}0, {r}1, {r}2"], res, nzcv(n, z, c, v), ini)
            res = addc(a, b, cin, w)[0]; add(regs(a, b) + [f"adc {r}0, {r}1, {r}2"], res, None, ini)
            res = addc(a, ~b, cin, w)[0]; add(regs(a, b) + [f"sbc {r}0, {r}1, {r}2"], res, None, ini)
        # conditional compare / select under every flag combo
        for fl in (0, 4, 2, 6, 8, 10, 1, 15):
            n, z, c, v = (fl >> 3) & 1, (fl >> 2) & 1, (fl >> 1) & 1, fl & 1
            ini = nzcv(n, z, c, v)
            for cc in ("eq", "ne", "hs" if False else "cs", "mi", "hi", "lt", "gt", "ge", "ls", "vs"):
                t = cond_true(cc, n, z, c, v)
                if t: _, n2, z2, c2, v2 = addc(a, ~b, 1, w)
                nz = 0b0101
                exp = nzcv(n2, z2, c2, v2) if t else nzcv((nz >> 3) & 1, (nz >> 2) & 1, (nz >> 1) & 1, nz & 1)
                add(regs(a, b) + [f"ccmp {r}1, {r}2, #{nz}, {cc}"], 0, exp, ini)
                if t: _, n2, z2, c2, v2 = addc(a, b, 0, w)
                exp = nzcv(n2, z2, c2, v2) if t else nzcv(1, 0, 1, 0)
                add(regs(a, b) + [f"ccmn {r}1, {r}2, #10, {cc}"], 0, exp, ini)
                imm = b & 31
                if t: _, n2, z2, c2, v2 = addc(a, ~imm, 1, w)
                exp = nzcv(n2, z2, c2, v2) if t else nzcv(0, 1, 0, 1)
                add(regs(a, b) + [f"ccmp {r}1, #{imm}, #5, {cc}"], 0, exp, ini)
                add(regs(a, b) + [f"csel {r}0, {r}1, {r}2, {cc}"], (a if t else b) & m, None, ini)
                add(regs(a, b) + [f"csinc {r}0, {r}1, {r}2, {cc}"], (a if t else b + 1) & m, None, ini)
                add(regs(a, b) + [f"csinv {r}0, {r}1, {r}2, {cc}"], (a if t else ~b) & m, None, ini)
                add(regs(a, b) + [f"csneg {r}0, {r}1, {r}2, {cc}"], (a if t else -b) & m, None, ini)
        # variable shifts / rotates
        for mn, f in (("lslv", lambda a, s: a << s), ("lsrv", lambda a, s: (a & m) >> s),
                      ("asrv", lambda a, s: sgn(a, w) >> s), ("rorv", lambda a, s: ror(a & m, s, w))):
            s = b % w
            add(regs(a, b) + [f"{mn} {r}0, {r}1, {r}2"], f(a, s) & m)
        # extract / rotate immediate
        for lsb in (0, 1, 7, w - 1):
            cat = ((a & m) << w | (b & m)); add(regs(a, b) + [f"extr {r}0, {r}1, {r}2, #{lsb}"], (cat >> lsb) & m)
        # one-source
        v = a & m
        add(regs(a, b) + [f"rbit {r}0, {r}1"], int(format(v, f"0{w}b")[::-1], 2))
        add(regs(a, b) + [f"clz {r}0, {r}1"], w - v.bit_length())
        sb = (v >> (w - 1)) & 1; cls = 0
        for k in range(w - 2, -1, -1):
            if (v >> k) & 1 == sb: cls += 1
            else: break
        add(regs(a, b) + [f"cls {r}0, {r}1"], cls)
        add(regs(a, b) + [f"rev {r}0, {r}1"], int.from_bytes(v.to_bytes(w // 8, "little"), "big"))
        h = sum((int.from_bytes(((v >> (16 * k)) & 0xFFFF).to_bytes(2, "little"), "big")) << (16 * k) for k in range(w // 16))
        add(regs(a, b) + [f"rev16 {r}0, {r}1"], h)
        if sf:
            add(regs(a, b) + ["rev32 x0, x1"], sum(int.from_bytes(((v >> (32 * k)) & M32).to_bytes(4, "little"), "big") << (32 * k) for k in range(2)))
        # bitfield aliases
        for lsb, wd in ((0, 8), (3, 5), (w - 8, 8), (0, 16), (5, 1), (1, w - 1)):
            if lsb + wd > w: continue
            fm = (1 << wd) - 1
            add(regs(a, b) + [f"ubfx {r}0, {r}1, #{lsb}, #{wd}"], (v >> lsb) & fm)
            add(regs(a, b) + [f"sbfx {r}0, {r}1, #{lsb}, #{wd}"], sgn((v >> lsb) & fm, wd) & m)
            if lsb:
                add(regs(a, b) + [f"ubfiz {r}0, {r}1, #{lsb}, #{wd}"], ((v & fm) << lsb) & m)
                add(regs(a, b) + [f"sbfiz {r}0, {r}1, #{lsb}, #{wd}"], (sgn(v & fm, wd) << lsb) & m)
            old = b & m
            add(regs(a, b) + [f"mov {r}0, {r}2", f"bfi {r}0, {r}1, #{lsb}, #{wd}"] if lsb else regs(a, b) + [f"mov {r}0, {r}2", f"bfxil {r}0, {r}1, #{lsb}, #{wd}"],
                ((old & ~(fm << lsb)) | ((v & fm) << lsb)) & m if lsb else ((old & ~fm) | ((v >> lsb) & fm)) & m)
        add(regs(a, b) + [f"sxtb {r}0, w1"], sgn(v, 8) & m)
        add(regs(a, b) + [f"sxth {r}0, w1"], sgn(v, 16) & m)
        add(regs(a, b) + [f"uxtb w0, w1"], v & 0xFF)
        add(regs(a, b) + [f"uxth w0, w1"], v & 0xFFFF)
        for s in (1, 13, w - 1):
            add(regs(a, b) + [f"lsl {r}0, {r}1, #{s}"], (v << s) & m)
            add(regs(a, b) + [f"lsr {r}0, {r}1, #{s}"], v >> s)
            add(regs(a, b) + [f"asr {r}0, {r}1, #{s}"], (sgn(v, w) >> s) & m)
            add(regs(a, b) + [f"ror {r}0, {r}1, #{s}"], ror(v, s, w))
        # logical immediates (clang encodes the bitmask immediates)
        for imm in (0xFF, 0xFF00FF00FF00FF00 if sf else 0xFF00FF00, 0x5555555555555555 if sf else 0x55555555, 0x3, 0x7FFFFFFE, 0xFFFF0000):
            add(regs(a, b) + [f"and {r}0, {r}1, #{imm}"], v & imm)
            add(regs(a, b) + [f"orr {r}0, {r}1, #{imm}"], (v | imm) & m)
            add(regs(a, b) + [f"eor {r}0, {r}1, #{imm}"], (v ^ imm) & m)
            res = v & imm; add(regs(a, b) + [f"ands {r}0, {r}1, #{imm}"], res, nzcv((res >> (w - 1)) & 1, int(res == 0), 0, 0))
        # extended-register add/sub
        for ext, wd, sg in (("uxtb", 8, 0), ("sxth", 16, 1), ("uxtw", 32, 0), ("sxtw", 32, 1), ("sxtb", 8, 1)):
            for sh in (0, 2, 4):
                x = b & ((1 << wd) - 1); x = sgn(x, wd) if sg else x
                add(["mov x1, #%d" % a, "mov x2, #%d" % b, f"add x0, x1, w2, {ext} #{sh}"], (a + (x << sh)) & M64)
                add(["mov x1, #%d" % a, "mov x2, #%d" % b, f"sub x0, x1, w2, {ext} #{sh}"], (a - (x << sh)) & M64)
        add(["mov x1, #%d" % a, "mov x2, #%d" % b, "add x0, sp, x2", "mov x3, sp", "sub x0, x0, x3"], b & M64)

# multiplies
for a, b, c in zip(vals(16), vals(16), vals(16)):
    ws = lambda v: sgn(v, 32)
    base = [f"mov x1, #{a}", f"mov x2, #{b}", f"mov x3, #{c}"]
    add(base + ["madd x0, x1, x2, x3"], (a * b + c) & M64)
    add(base + ["msub x0, x1, x2, x3"], (c - a * b) & M64)
    add(base + ["madd w0, w1, w2, w3"], (a * b + c) & M32)
    add(base + ["msub w0, w1, w2, w3"], (c - a * b) & M32)
    add(base + ["smaddl x0, w1, w2, x3"], (ws(a) * ws(b) + c) & M64)
    add(base + ["smsubl x0, w1, w2, x3"], (c - ws(a) * ws(b)) & M64)
    add(base + ["umaddl x0, w1, w2, x3"], ((a & M32) * (b & M32) + c) & M64)
    add(base + ["umsubl x0, w1, w2, x3"], (c - (a & M32) * (b & M32)) & M64)
    add(base + ["smulh x0, x1, x2"], (sgn(a, 64) * sgn(b, 64)) >> 64 & M64)
    add(base + ["umulh x0, x1, x2"], (a * b) >> 64 & M64)
    if b: add(base + ["udiv x0, x1, x2"], a // b)
    if b & M32: add(base + ["udiv w0, w1, w2"], (a & M32) // (b & M32))
    add(base + ["mov x4, #0", "udiv x0, x1, x4"], 0)
    d = sgn(b, 64)
    if d not in (0, -1): add(base + ["sdiv x0, x1, x2"], int(sgn(a, 64) / d) & M64 if abs(sgn(a, 64)) < 2**52 else (abs(sgn(a, 64)) // abs(d) * (1 if (sgn(a, 64) < 0) == (d < 0) else -1)) & M64)

import re
def expand(l):
    m = re.fullmatch(r"\s*mov (x\d+), #(\d+)", l)
    if not m: return [l]
    reg, v = m.group(1), int(m.group(2)); ch = [(k, (v >> (16 * k)) & 0xFFFF) for k in range(4)]
    nz = [c for c in ch if c[1]]
    if not nz: return [f"movz {reg}, #0"]
    res = [f"movz {reg}, #{nz[0][1]}, lsl #{16 * nz[0][0]}"]
    res += [f"movk {reg}, #{val}, lsl #{16 * k}" for k, val in nz[1:]]
    return res
out = []
def P(t):
    for l in t.split("\n"):
        out.extend(expand(l))
P("// GENERATED by tools/gen_asm_tests.py - do not edit.\n.text\n.globl _start\n_start:")
for idx, (lines, x0, fl, ini) in enumerate(cases):
    P(f"// case {idx}\n    mov x0, #0")
    if ini is not None: P(f"    mov x20, #{ini}\n    msr nzcv, x20")
    for l in lines: P("    " + l)
    P("    mrs x9, nzcv")
    P(f"    mov x10, #{x0}\n    cmp x0, x10\n    b.ne fail{idx}")
    if fl is not None: P(f"    mov x10, #{fl}\n    cmp x9, x10\n    b.ne fail{idx}")
    P(f"    b ok{idx}\nfail{idx}:\n    mov x0, #{idx + 1}\n    b out\nok{idx}:")
P("    mov x0, #0\nout:\n    mov x8, #93\n    svc #0")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
print(len(cases), "cases", file=sys.stderr)
