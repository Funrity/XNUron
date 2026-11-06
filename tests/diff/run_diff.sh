#!/bin/sh
# Differential test: same C program on host (reference) vs inside the XNUron core.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); OUT=${OUT:-$(mktemp -d)}
CC=${CC:-clang}; LLD=${LLD:-ld.lld}; OBJCOPY=${OBJCOPY:-llvm-objcopy}
gcc -O2 -w -o "$OUT/runbin" "$ROOT/tests/runbin.c" "$ROOT/app/src/main/cpp/arm64.c" -lm
gcc -O1 -w -o "$OUT/ref" "$ROOT/tests/diff/diffprog.c"
"$OUT/ref" > "$OUT/ref.txt"
fail=0
for opt in -O0 -O1 -O2 -Os -O3; do
  $CC --target=aarch64-none-elf -ffreestanding -fno-builtin -mgeneral-regs-only -fno-stack-protector -fno-pic $opt -w -c \
      "$ROOT/tests/diff/diffprog.c" -o "$OUT/g.o"
  $LLD --image-base=0 -static -nostdlib -Ttext=0x10000 -e _start "$OUT/g.o" -o "$OUT/g.elf"
  $OBJCOPY -O binary "$OUT/g.elf" "$OUT/g.bin"
  if "$OUT/runbin" "$OUT/g.bin" > "$OUT/g$opt.txt" 2> "$OUT/err.txt" && cmp -s "$OUT/ref.txt" "$OUT/g$opt.txt"; then
    echo "ok:   differential $opt ($(tr '\n' ' ' < "$OUT/err.txt"))"
  else
    echo "FAIL: differential $opt"; cat "$OUT/err.txt"; diff "$OUT/ref.txt" "$OUT/g$opt.txt" | head -5; fail=1
  fi
done

# SIMD / floating point: same program, compiler free to use NEON (no -mgeneral-regs-only), FMA contraction off so the host reference matches
gcc -O1 -w -ffp-contract=off -fno-math-errno -o "$OUT/sref" "$ROOT/tests/diff/simdprog.c"
"$OUT/sref" > "$OUT/sref.txt"
for opt in -O0 -O1 -O2 -O3; do
  $CC --target=aarch64-none-elf -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -ffp-contract=off -fno-math-errno $opt -w -c \
      "$ROOT/tests/diff/simdprog.c" -o "$OUT/s.o"
  $LLD --image-base=0 -static -nostdlib -Ttext=0x10000 -e _start "$OUT/s.o" -o "$OUT/s.elf"
  $OBJCOPY -O binary "$OUT/s.elf" "$OUT/s.bin"
  if "$OUT/runbin" "$OUT/s.bin" > "$OUT/s$opt.txt" 2> "$OUT/err.txt" && cmp -s "$OUT/sref.txt" "$OUT/s$opt.txt"; then
    echo "ok:   differential SIMD/FP $opt ($(tr '\n' ' ' < "$OUT/err.txt"))"
  else
    echo "FAIL: differential SIMD/FP $opt"; cat "$OUT/err.txt"; diff "$OUT/sref.txt" "$OUT/s$opt.txt" | head -5; fail=1
  fi
done
exit $fail
