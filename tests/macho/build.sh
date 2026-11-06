#!/bin/sh
# usage: build.sh out.macho   (own test program linked as a real arm64 iOS-platform Mach-O)
set -e
D=$(cd "$(dirname "$0")" && pwd); T=$(mktemp -d)
LD=${LD64:-$(ls /usr/lib/llvm-*/bin/ld64.lld 2>/dev/null | head -1)}
clang --target=arm64-apple-ios14.0 -c "$D/hello.S" -o "$T/h.o"
"$LD" -arch arm64 -platform_version ios 14.0 14.0 -e _start -o "$1" "$T/h.o"
