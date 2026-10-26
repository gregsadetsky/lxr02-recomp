#!/bin/sh
# make a blank 64 MB FAT32 sd card image (raw sectors, no partition table, the
# way macOS formats a card) with hdiutil. no sudo. usage: tools/mkcard.sh out.img
set -eu
OUT=${1:?out.img}; T=$(mktemp -d)
hdiutil create -size 64m -fs "MS-DOS FAT32" -volname LXR -layout NONE -type UDIF "$T/card.dmg" > /dev/null
hdiutil convert "$T/card.dmg" -format UDTO -o "$T/card" > /dev/null
mv "$T/card.cdr" "$OUT"; rm -rf "$T"
echo "$OUT: $(wc -c < "$OUT") bytes"
