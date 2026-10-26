#!/bin/sh
# web build: translate the arm elf (2.engine/build/arm/lxr.elf: erica's firmware + the
# virtual board, arm gnu, erica's flags) to wasm for a worker. usage: ./build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd); R=$ROOT/3.recomp
G=$HERE/build/gen; mkdir -p "$G" "$HERE/site"
uv run --project "$R" python "$R/backend.py" "$ROOT/2.engine/build/arm/lxr.elf" "$G"
CF="-O2 -std=gnu11 -fno-fast-math -ffp-contract=off -I$R/rt -DRT_WEB"
emcc $CF -w -fbracket-depth=1024 -c "$G/fw.c" -o "$G/fw.o"
emcc $CF -w -c "$G/image.c" -o "$G/image.o"
emcc $CF -Wall -c "$R/rt/rt.c" -o "$G/rt.o"
emcc $CF -Wall -c "$R/rt/fp.c" -o "$G/fp.o"
emcc $CF -Wall -c "$R/rt/fz.c" -o "$G/fz.o"
emcc "$G/fw.o" "$G/image.o" "$G/rt.o" "$G/fp.o" "$G/fz.o" -o "$HERE/site/lxr.js" \
  -sENVIRONMENT=worker -sMODULARIZE=1 -sEXPORT_NAME=LXR -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 \
  -sEXPORTED_RUNTIME_METHODS=FS,callMain,HEAPU8 -sINITIAL_MEMORY=160MB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1
gzip -9 -n -c "$ROOT/2.engine/cards/kit-zippe.img" > "$HERE/site/card.img.gz"
# comparison engine: erica's sources built by clang directly (2.engine/build.sh wasm)
"$ROOT/2.engine/build.sh" wasm > "$HERE/build/plain-build.log" 2>&1 || { tail -20 "$HERE/build/plain-build.log"; exit 1; }
mkdir -p "$HERE/site/plain"; cp "$ROOT/2.engine/build/wasm/lxr-web.js" "$ROOT/2.engine/build/wasm/lxr-web.wasm" "$HERE/site/plain/"
grep -v ' end' "$ROOT/2.engine/scenario/live-zippe.in" > "$HERE/site/test-zippe.in"
ls -la "$HERE/site"
