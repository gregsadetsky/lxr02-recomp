#!/bin/sh
# translate the arm elf and build a native host binary of it (for fast checking against qemu).
# usage: ./build-native.sh <arm elf> <outdir>
set -eu
ELF=${1:?elf}; OUT=${2:?outdir}
HERE=$(cd "$(dirname "$0")" && pwd)
uv run --project "$HERE" python "$HERE/backend.py" "$ELF" "$OUT"
CF="-O1 -std=gnu11 -fno-fast-math -ffp-contract=off -I$HERE/rt ${RT_CFLAGS:-}"
clang $CF -w -fbracket-depth=1024 -c "$OUT/fw.c" -o "$OUT/fw.o"
clang $CF -w -c "$OUT/image.c" -o "$OUT/image.o"
clang $CF -Wall -c "$HERE/rt/rt.c" -o "$OUT/rt.o"
clang $CF -Wall -c "$HERE/rt/fp.c" -o "$OUT/fp.o"
clang "$OUT/fw.o" "$OUT/image.o" "$OUT/rt.o" "$OUT/fp.o" -o "$OUT/lxr-native" 2>&1 | grep -v 'reducing alignment' || true
echo "$OUT/lxr-native"
