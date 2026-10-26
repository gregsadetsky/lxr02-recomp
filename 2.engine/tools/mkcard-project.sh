#!/bin/sh
# a card with several kits as project 0, slots 00, 01, ... (same layout as mkcard-kit.sh, one
# .snd per slot, named NN-<5 chars>.SND). usage: tools/mkcard-project.sh blank.img out.img kit.snd...
set -eu
BLANK=${1:?blank.img}; OUT=${2:?out.img}; shift 2
export MTOOLS_SKIP_CHECK=1
HERE=$(cd "$(dirname "$0")/.." && pwd)
"$HERE/tools/mkcard-kit.sh" "$BLANK" "$1" "$OUT" > /dev/null      # slot 00 + project files + glo.cfg
n=0
for k in "$@"; do
  if [ $n -gt 0 ]; then
    NAME=$(basename "$k" | sed -E 's/^[0-9]+-//; s/\.[sS][nN][dD]$//; s/-.*//' | cut -c1-5 | tr a-z A-Z)
    mcopy -i "$OUT" "$k" "::/PROJ00/$(printf %02d $n)-$NAME.SND"
  fi
  n=$((n+1))
done
mdir -b -i "$OUT" ::/PROJ00
