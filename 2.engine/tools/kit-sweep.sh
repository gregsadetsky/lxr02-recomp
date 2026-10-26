#!/bin/sh
# one card + scenario per kit file (slot 00 of project 0, voices on notes 36..42), so every
# kit of a corpus can be run on qemu and on the translation. usage: tools/kit-sweep.sh kit.snd...
# writes cards/sweep/<n>.img and scenario/sweep/<n>.txt, prints the scenario names.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd); cd "$HERE"
mkdir -p cards/sweep scenario/sweep
for k in "$@"; do
  n=$(basename "$k" | tr 'A-Z ' 'a-z_' | sed 's/\.snd$//; s/[^a-z0-9_-]/_/g')
  n=$(printf '%s-%s' "$(printf '%s' "$k" | shasum | cut -c1-6)" "$n")
  tools/mkcard-kit.sh cards/blank-fat32-64m.img "$k" "cards/sweep/$n.img" > /dev/null
  { echo "# kit sweep: $k"; echo "card cards/sweep/$n.img"; echo "seed c0ffee"
    t=5000; for note in 24 25 26 27 28 29 2a; do echo "$t midi 90 $note 7f"; t=$((t+400)); done
    echo "$t midi 90 24 64"; echo "$t midi 90 2a 50"; t=$((t+200)); echo "$t midi 90 26 70"; t=$((t+200)); echo "$t midi 90 29 60"
    echo "$((t+1200)) end"; } > "scenario/sweep/$n.txt"
  echo "sweep/$n"
done
