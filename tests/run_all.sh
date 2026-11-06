#!/bin/sh
# Runs every host-side test. Needs: gcc, clang, ld.lld, llvm-objcopy, python3; java+javac optional (JNI smoke test).
ROOT=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); C="$ROOT/app/src/main/cpp"; fail=0
CF="-O2 -Wall -Wextra -Wno-misleading-indentation"
step() { printf '\n=== %s ===\n' "$1"; }
run()  { if ! "$@"; then fail=1; echo "!!! FAILED: $*"; fi; }

step "1. core unit tests (v0.1 tests, kept)"
gcc $CF -o "$T/t_core" "$ROOT/tests/test.c" "$C/arm64.c" -lm "$C/a64demo.c" && run "$T/t_core" | tail -3 | sed 's/^/  /'
"$T/t_core" >/dev/null || fail=1

step "2. per-instruction tests (13830 cases, expectations from tools/gen_asm_tests.py)"
python3 "$ROOT/tools/gen_asm_tests.py" "$T/insn.S" && gcc -O2 -w -o "$T/runbin" "$ROOT/tests/runbin.c" "$C/arm64.c" -lm || fail=1
for p in "$T/insn.S" "$ROOT/tests/asm/mem_tests.S" "$ROOT/tests/asm/pac_tests.S" "$ROOT/tests/asm/simd_tests.S" "$ROOT/tests/asm/lse_tests.S"; do
  b=$(basename "$p" .S)
  clang --target=aarch64-none-elf -c "$p" -o "$T/$b.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0x10000 -e _start "$T/$b.o" -o "$T/$b.elf" \
    && llvm-objcopy -O binary "$T/$b.elf" "$T/$b.bin" && "$T/runbin" "$T/$b.bin" 2>"$T/err" && echo "  ok: $b ($(cat "$T/err"))" || { echo "  FAIL: $b $(cat "$T/err")"; fail=1; }
done

step "2b. crypto extension: SHA-1 / SHA-256 / AES against FIPS test vectors (real clang-compiled NEON-intrinsics program)"
clang --target=aarch64-none-elf -O1 -fno-math-errno -ffreestanding -nostdlib -march=armv8-a+crypto -c "$ROOT/tests/asm/crypto_test.c" -o "$T/crypto_t.o" && clang --target=aarch64-none-elf -c "$ROOT/tests/asm/crypto_start.S" -o "$T/crypto_s.o" \
  && ld.lld --image-base=0 -static -nostdlib -Ttext=0x10000 -e _start "$T/crypto_s.o" "$T/crypto_t.o" -o "$T/crypto.elf" && llvm-objcopy -O binary "$T/crypto.elf" "$T/crypto.bin" \
  && "$T/runbin" "$T/crypto.bin" 2>"$T/err" && echo "  ok: crypto ($(cat "$T/err"))" || { echo "  FAIL: crypto $(cat "$T/err")"; fail=1; }

step "2c. AdvSIMD three-different + by-element forms (uaddw, umull, mla by element, fmla by element ...)"
clang --target=aarch64-none-elf -O1 -fno-math-errno -fno-vectorize -fno-slp-vectorize -ffreestanding -nostdlib -march=armv8-a+crypto -c "$ROOT/tests/asm/simd2_test.c" -o "$T/simd2_t.o" \
  && ld.lld --image-base=0 -static -nostdlib -Ttext=0x10000 -e _start "$T/crypto_s.o" "$T/simd2_t.o" -o "$T/simd2.elf" && llvm-objcopy -O binary "$T/simd2.elf" "$T/simd2.bin" \
  && "$T/runbin" "$T/simd2.bin" 2>"$T/err" && echo "  ok: simd2 ($(cat "$T/err"))" || { echo "  FAIL: simd2 $(cat "$T/err")"; fail=1; }

step "2d. MODELLED CNTP timer FIQ: masked / delivered / wakes WFI"
clang --target=aarch64-none-elf -c "$ROOT/tests/asm/fiq.S" -o "$T/fiq.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0x40000000 -e _start "$T/fiq.o" -o "$T/fiq.elf" && llvm-objcopy -O binary "$T/fiq.elf" "$T/fiq.bin" \
  && gcc $CF -o "$T/t_fiq" "$ROOT/tests/fiq_test.c" "$C/machine.c" "$C/arm64.c" -lm -lpthread && run "$T/t_fiq" "$T/fiq.bin"

step "3. differential test: host x86-64 vs emulated AArch64 (-O0..-O3)"
OUT="$T/diff" ; mkdir -p "$OUT"; OUT="$OUT" run sh "$ROOT/tests/diff/run_diff.sh"

step "4. system mode: a64virt + bundled test guest kernel"
clang --target=aarch64-none-elf -c "$ROOT/tests/asm/mmu.S" -o "$T/mmu.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0x40000000 -e _start "$T/mmu.o" -o "$T/mmu.elf" && llvm-objcopy -O binary "$T/mmu.elf" "$T/mmu.bin" \
  && gcc $CF -o "$T/t_machine" "$ROOT/tests/machine_test.c" "$C/machine.c" "$C/arm64.c" -lm -lpthread && run "$T/t_machine" "$T/mmu.bin"

step "5. loaders: Mach-O (real arm64 binary from lld), IMG4/IM4P, IPSW zip64"
"$ROOT/tests/macho/build.sh" "$T/hello.macho" && python3 "$ROOT/tests/zipgen.py" "$T/ipsw.zip" "$T/plain.zip" "$T/old.zip" \
  && gcc $CF -o "$T/t_loader" "$ROOT/tests/loader_test.c" "$C/macho.c" "$C/machine.c" "$C/arm64.c" -lm "$C/inspect.c" "$C/img4.c" "$C/zipinfo.c" -lpthread \
  && run "$T/t_loader" "$T/hello.macho" "$T/ipsw.zip" "$T/plain.zip" "$T/old.zip"

step "5b. IPSW boot attempt: IM4P -> LZFSE/LZSS -> arm64 kernel really executed (synthetic fixtures built here)"
LZ="$ROOT/app/src/main/cpp/lzfse"
gcc -O2 -w -I"$ROOT/tests/lzfse_enc" -o "$T/lzenc" "$ROOT"/tests/lzfse_enc/*.c \
  && clang --target=aarch64-none-elf -c "$ROOT/tests/attempt/kernel.S" -o "$T/k.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0 -e _start "$T/k.o" -o "$T/k.elf" \
  && llvm-objcopy -O binary "$T/k.elf" "$T/k.bin" && clang --target=aarch64-none-elf -c "$ROOT/tests/attempt/spin.S" -o "$T/s.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0 -e _start "$T/s.o" -o "$T/s.elf" \
  && llvm-objcopy -O binary "$T/s.elf" "$T/s.bin" \
  && clang --target=aarch64-none-elf -c "$ROOT/tests/attempt/vecspin.S" -o "$T/v.o" && ld.lld --image-base=0 -static -nostdlib -Ttext=0 -e _start "$T/v.o" -o "$T/v.elf" && llvm-objcopy -O binary "$T/v.elf" "$T/v.bin" \
  && mkdir -p "$T/fx" && python3 -I "$ROOT/tests/ipswgen.py" "$T/fx" "$T/lzenc" "$T/k.bin" "$T/s.bin" "$T/v.bin" \
  && gcc -O2 -Wall -Wno-misleading-indentation -Wno-sign-compare -o "$T/t_att" "$ROOT/tests/attempt_test.c" "$C/attempt.c" "$C/macho.c" "$C/machine.c" "$C/arm64.c" -lm "$C/inspect.c" "$C/img4.c" "$C/zipinfo.c" \
     "$LZ/lzfse_decode.c" "$LZ/lzfse_decode_base.c" "$LZ/lzfse_fse.c" "$LZ/lzvn_decode_base.c" -lz -lpthread \
  && run "$T/t_att" "$T/fx" || fail=1

if command -v javac >/dev/null 2>&1; then
  step "6. JNI glue from a plain JVM (same C sources as the APK)"
  JH=${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}
  mkdir -p "$T/jni/o"
  for f in arm64 a64demo machine macho inspect img4 zipinfo attempt lzfse/lzfse_decode lzfse/lzfse_decode_base lzfse/lzfse_fse lzfse/lzvn_decode_base; do mkdir -p "$T/jni/o";  gcc -fPIC -O2 -w -c "$C/$f.c" -I"$C" -o "$T/jni/o/$(basename $f).o"; done
  g++ -fPIC -O2 -w -I"$JH/include" -I"$JH/include/linux" -c "$C/native-lib.cpp" -o "$T/jni/o/native.o" \
    && g++ -shared -o "$T/jni/liba64.so" "$T/jni/o"/*.o -lpthread -lz \
    && javac -d "$T/jni" "$ROOT/app/src/main/java/com/xnuron/app/Native.java" "$ROOT/tests/jni/JniSmoke.java" 2>/dev/null \
    && { java --add-opens java.base/java.io=ALL-UNNAMED -Djava.library.path="$T/jni" -cp "$T/jni" JniSmoke "$T/hello.macho" "$T/ipsw.zip" "$T/fx/ok_lzfse.zip" > "$T/jni.out" 2>&1; rc=$?; grep -v JAVA_TOOL "$T/jni.out"; [ $rc = 0 ] || { fail=1; echo "!!! FAILED: JniSmoke"; }; }
else echo; echo "(javac not found: JNI smoke test skipped)"; fi

echo; [ $fail = 0 ] && echo "ALL TEST GROUPS PASSED" || echo "SOME TESTS FAILED"
exit $fail
