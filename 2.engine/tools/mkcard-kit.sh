#!/bin/sh
# make a card image holding one kit as project 0, slot 00, laid out the way the
# firmware's own project save does (presetManager.c preset_saveProject):
#   /PROJ.IDX            1 byte, last used project (0)
#   /PROJ00/PRJ.NFO      8 byte project name
#   /PROJ00/GLO.CFG      30 bytes, activeParameters[PAR_BEGINNING_OF_GLOBALS..NUM_PARAMS)
#   /PROJ00/00-NAME.SND  the kit file, unchanged
# glo.cfg starts from cards/src/glo-firmware-default.cfg (written by the firmware
# itself on a blank card boot) with only PAR_MIDI_NOTE1..7 (bytes 22..28) set to
# 36..42, i.e. what editing the per-voice midi notes in the menu and saving gives.
# usage: tools/mkcard-kit.sh blank.img kit.snd out.img   (slot 00, named 00-<5 chars of the kit name>.SND)
set -eu
BLANK=${1:?blank.img}; KIT=${2:?kit.snd}; OUT=${3:?out.img}
export MTOOLS_SKIP_CHECK=1
HERE=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d)
cp "$BLANK" "$OUT"
printf '\000' > "$T/proj.idx"
printf 'web     ' > "$T/prj.nfo"
GLO_PITCH=${GLO_PITCH:-0} uv run --no-project python - "$HERE/cards/src/glo-firmware-default.cfg" "$T/glo.cfg" <<'PY'
import sys
g = bytearray(open(sys.argv[1], "rb").read())
assert len(g) == 30 and g[0] == 120 and g[22:29] == b"\x30" * 7, g.hex()
import os
# default: every voice on channel 1, notes 36..42. GLO_PITCH=1: voice i on channel i+2
# (PAR_MIDI_CHAN_1..6 at bytes 1..6, PAR_MIDI_CHAN_7 at 12) answering any note (0), so the
# note number sets the pitch.
if os.environ.get("GLO_PITCH") == "1":
    g[1:7] = bytes(range(2, 8)); g[12] = 8; g[22:29] = bytes(7)
else:
    g[22:29] = bytes(range(36, 43))
open(sys.argv[2], "wb").write(g)
PY
mmd -i "$OUT" ::/PROJ00
mcopy -i "$OUT" "$T/proj.idx" ::/PROJ.IDX
mcopy -i "$OUT" "$T/prj.nfo" ::/PROJ00/PRJ.NFO
mcopy -i "$OUT" "$T/glo.cfg" ::/PROJ00/GLO.CFG
NAME=$(basename "$KIT" | sed -E 's/^[0-9]+-//; s/\.[sS][nN][dD]$//; s/-.*//' | cut -c1-5 | tr a-z A-Z)
mcopy -i "$OUT" "$KIT" "::/PROJ00/00-$NAME.SND"
rm -rf "$T"
