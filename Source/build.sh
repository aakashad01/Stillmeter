#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
mkdir -p build

# Requires LLVM-MinGW 20251104 UCRT or a newer compatible release on PATH.
for arch in x86_64 aarch64; do
    target="$arch-w64-mingw32"
    "$target-windres" -O coff -i stillmeter.rc -o "build/stillmeter-$arch.o"
    "$target-clang" -O2 -s -municode -mwindows -std=c17 -Wall -Wextra -Werror \
        -fstack-protector-strong -mguard=cf -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE \
        app.c monitor.c meter.c view.c "build/stillmeter-$arch.o" \
        -Wl,--dynamicbase,--nxcompat,--high-entropy-va,--guard-cf,--dependent-load-flag=0x800,--no-insert-timestamp \
        -luser32 -lgdi32 -lshell32 -o "build/Stillmeter-$arch.exe"
done
