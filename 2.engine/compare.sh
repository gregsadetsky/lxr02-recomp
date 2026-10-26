#!/bin/sh
# compare two run directories file by file: ./compare.sh <scenario> <target a> <target b>
set -u
HERE=$(cd "$(dirname "$0")" && pwd); S=$1; A="$HERE/out/$2/$S"; B="$HERE/out/$3/$S"; ok=1
for x in dac1.raw dac2.raw lcd.txt midiout.txt params.txt stdout.txt; do
  if cmp -s "$A/$x" "$B/$x"; then r=identical; else r=DIFFERS; ok=0; fi
  printf '%-12s %s\n' "$x" "$r"
done
if [ -f "$A/card.img" ] || [ -f "$B/card.img" ]; then
  if cmp -s "$A/card.img" "$B/card.img"; then echo "card.img     identical"; else echo "card.img     DIFFERS"; ok=0; fi
fi
[ $ok = 1 ] && echo "$S: $2 == $3" || { echo "$S: $2 != $3"; exit 1; }
