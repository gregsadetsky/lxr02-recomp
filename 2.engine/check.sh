#!/bin/sh
# regression check: the translated firmware (recompw) against golden hashes of qemu's
# cortex-m7 runs. scenario/regress.txt lists the scenarios, scenario/golden/ holds one
# sha256 file per scenario (dac1, dac2, lcd, midi out, params).
#   ./check.sh          build recompw, run every listed scenario, compare with the goldens
#   ./check.sh update   run the listed scenarios on qemu and (re)write the goldens
# env: NOBUILD=1 skips the build, JOBS (default 8) for qemu.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
LIST=scenario/regress.txt; G=scenario/golden
FILES="dac1.raw dac2.raw lcd.txt midiout.txt params.txt"
hashes() { (cd "$1" && shasum -a 256 $FILES); }
if [ "${1:-}" = update ]; then
  ./build.sh arm > build/arm-build.log 2>&1 || { echo "arm build failed"; exit 1; }
  grep -v '^#' "$LIST" | grep -v '^$' | xargs -P "${JOBS:-8}" -I{} sh -c 'NOBUILD=1 ./run.sh qemu {} > /dev/null 2>&1 || echo "qemu failed: {}"'
  grep -v '^#' "$LIST" | grep -v '^$' | while read -r s; do
    mkdir -p "$G/$(dirname "$s")"; hashes "out/qemu/$s" > "$G/$s.sha256"
  done
  echo "goldens written for $(grep -v '^#' "$LIST" | grep -c .) scenarios"; exit 0
fi
if [ "${NOBUILD:-0}" != 1 ]; then
  ./build.sh arm > build/arm-build.log 2>&1 || { echo "arm build failed"; exit 1; }
  ../3.recomp/build-wasm.sh build/arm/lxr.elf build/recompw > build/recompw-build.log 2>&1 || { echo "recomp build failed"; exit 1; }
fi
pass=0; fail=0
for s in $(grep -v '^#' "$LIST" | grep -v '^$'); do
  [ -f "$G/$s.sha256" ] || { echo "MISSING GOLDEN: $s"; fail=$((fail+1)); continue; }
  NOBUILD=1 ./run.sh recompw "$s" > /dev/null 2>&1
  if hashes "out/recompw/$s" | cmp -s - "$G/$s.sha256"; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFFERS: $s"; fi
done
echo "check: $pass identical to the cortex-m7 goldens, $fail not"
[ "$fail" = 0 ]
