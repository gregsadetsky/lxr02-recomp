#!/bin/sh
# translate the arm elf and build it as wasm for node (files through NODERAWFS).
# usage: ./build-wasm.sh <arm elf> <outdir>   ->  <outdir>/lxr.js + lxr.wasm
set -eu
ELF=${1:?elf}; OUT=${2:?outdir}
HERE=$(cd "$(dirname "$0")" && pwd)
uv run --project "$HERE" python "$HERE/backend.py" "$ELF" "$OUT"
CF="-O2 -std=gnu11 -fno-fast-math -ffp-contract=off -I$HERE/rt ${RT_CFLAGS:-}"
emcc $CF -w -fbracket-depth=1024 -c "$OUT/fw.c" -o "$OUT/fw.wasm.o"
emcc $CF -w -c "$OUT/image.c" -o "$OUT/image.wasm.o"
emcc $CF -Wall -c "$HERE/rt/rt.c" -o "$OUT/rt.wasm.o"
emcc $CF -Wall -c "$HERE/rt/fp.c" -o "$OUT/fp.wasm.o"
emcc "$OUT/fw.wasm.o" "$OUT/image.wasm.o" "$OUT/rt.wasm.o" "$OUT/fp.wasm.o" -o "$OUT/lxr.js" \
  -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=4MB
echo "$OUT/lxr.js"
