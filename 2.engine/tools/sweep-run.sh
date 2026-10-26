#!/bin/sh
# run scenarios on qemu (parallel) and on the wasm translation, compare each pair.
# usage: tools/sweep-run.sh <file with one scenario name per line> [jobs]
# idempotent: a scenario whose qemu run finished (stdout has "done at") is not rerun.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); cd "$HERE"
L=${1:?list}; J=${2:-8}
./build.sh arm > build/arm-build.log 2>&1 || { echo "arm build failed"; exit 1; }
../3.recomp/build-wasm.sh build/arm/lxr.elf build/recompw > build/recompw-build.log 2>&1 || { echo "recomp build failed"; exit 1; }
grep -v '^$' "$L" | while read -r s; do
  grep -q 'done at' "out/qemu/$s/stdout.txt" 2>/dev/null || echo "$s"
done | xargs -P "$J" -I{} sh -c 'NOBUILD=1 ./run.sh qemu {} > /dev/null 2>&1 || echo "qemu failed: {}"'
pass=0; fail=0
while read -r s; do
  NOBUILD=1 ./run.sh recompw "$s" > /dev/null 2>&1
  if ./compare.sh "$s" qemu recompw > /dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFFERS: $s"; fi
done < "$L"
echo "sweep: $pass identical, $fail differ"
