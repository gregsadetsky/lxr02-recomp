#!/bin/sh
# run one scenario in its own directory: out/<target>/<scenario>/ holds scenario.txt,
# card.img (a copy) and every output. the firmware image does not depend on the scenario.
#   ./run.sh qemu <scenario>     arm gnu build on qemu mps2-an500 (cortex-m7): the reference
#   ./run.sh recompw <scenario>  that same elf translated to wasm (3.recomp), node: the product
#   ./run.sh recomp <scenario>   the translation built natively (fast checks)
#   ./run.sh wasm <scenario>     erica's sources built by clang for wasm directly (comparison)
# env: TEST_CFLAGS (experiments only), TAG (suffix for the output directory), NOBUILD=1
set -eu
T=${1:?qemu|recompw|recomp|wasm}; S=${2:?scenario}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd); cd "$HERE"
SC="$HERE/scenario/$S.txt"; [ -f "$SC" ] || { echo "no $SC"; exit 1; }
case $T in qemu|recomp|recompw) BT=arm;; wasm) BT=wasm;; *) echo "target?"; exit 1;; esac
if [ "${NOBUILD:-0}" != 1 ]; then
  ./build.sh "$BT" > "build/$BT-build.log" 2>&1 || { tail -20 "build/$BT-build.log"; exit 1; }
  case $T in
    recomp)  "$ROOT/3.recomp/build-native.sh" build/arm/lxr.elf build/recomp > build/recomp-build.log 2>&1 || { tail -20 build/recomp-build.log; exit 1; };;
    recompw) "$ROOT/3.recomp/build-wasm.sh" build/arm/lxr.elf build/recompw > build/recompw-build.log 2>&1 || { tail -20 build/recompw-build.log; exit 1; };;
  esac
fi
D="$HERE/out/$T${TAG:+-$TAG}/$S"; rm -rf "$D"; mkdir -p "$D"
cp "$SC" "$D/scenario.txt"
card=$(awk '$1 == "card" { print $2 }' "$SC")
[ -n "$card" ] && cp "$HERE/$card" "$D/card.img"
[ -f "$HERE/scenario/$S.in" ] && cp "$HERE/scenario/$S.in" "$D/live.in"
cd "$D"
# qemu's semihosting command line is the -kernel path; the translation reports the same
# string ("lxr.elf"), so crt0 builds the same argv on the stack in both.
case $T in
  qemu)    ln -s "$HERE/build/arm/lxr.elf" lxr.elf
           timeout 1200 qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -semihosting-config enable=on,target=native -kernel lxr.elf > stdout.txt 2> stderr.txt;;
  recomp)  "$HERE/build/recomp/lxr-native" > stdout.txt 2> stderr.txt;;
  recompw) node "$HERE/build/recompw/lxr.js" > stdout.txt 2> stderr.txt;;
  wasm)    node "$HERE/build/wasm/lxr.js" > stdout.txt 2> stderr.txt;;
esac
tail -1 stdout.txt
