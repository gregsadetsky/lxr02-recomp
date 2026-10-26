#!/bin/sh
# build the whole firmware (erica's 87 translation units minus the hardware layer,
# see tools/firmware-sources.py) on the virtual board.
#   ./build.sh wasm   -> emscripten, run with node
#   ./build.sh arm    -> arm gnu 14.3.rel1, run on qemu mps2-an500 (cortex-m7)
# env: ONLY_COMPILE=1 stops after compiling (no link).
set -eu
TARGET=${1:?wasm or arm}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
U="$ROOT/upstream/lxr-desktop-app"
CCDB="$ROOT/build-fw/release/compile_commands.json"
[ -f "$CCDB" ] || { echo "missing $CCDB: build erica's firmware first (see readme)"; exit 1; }
B="$HERE/build/$TARGET"; mkdir -p "$B/obj"
# objects depend on flags: start clean when the flag set changes
FLAGSIG="${TEST_CFLAGS:-}|$TARGET"; [ -f "$B/flags" ] && [ "$(cat "$B/flags")" != "$FLAGSIG" ] && rm -f "$B"/obj/*.o; echo "$FLAGSIG" > "$B/flags"
python3 -I "$HERE/tools/gen-remap.py" "$U/Drivers/CMSIS/Device/ST/STM32F7xx/Include/stm32f765xx.h" "$HERE/shim/lxr_remap.h" "$HERE/platform/lxr_remap.c" > /dev/null
INC="-I$HERE/shim"
if [ "$TARGET" = wasm ]; then
  python3 -I "$HERE/tools/gen-core-wasm.py" "$U/Drivers/CMSIS/Include" "$HERE/shim-wasm" > /dev/null
  INC="$INC -I$HERE/shim-wasm"
  CC=emcc
  # arm-none-eabi abi defaults made explicit for wasm (measured in 1.dsp-wasm-poc)
  TFLAGS="-funsigned-char -fshort-enums"
else
  TC="$ROOT/tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin"
  CC="$TC/arm-none-eabi-gcc"
  TFLAGS="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -mfp16-format=ieee --specs=nano.specs"
fi
INC="$INC -I$U/Core/Inc -I$U/Drivers/STM32F7xx_HAL_Driver/Inc -I$U/Drivers/STM32F7xx_HAL_Driver/Inc/Legacy
 -I$U/Drivers/CMSIS/Device/ST/STM32F7xx/Include -I$U/Drivers/CMSIS/Include
 -I$U/Middlewares/Third_Party/FatFs/src -I$U/FATFS/App -I$U/FATFS/Target
 -I$U/Middlewares/JS/STM32_USB_Device_Library/Core/Inc -I$U/Middlewares/JS/STM32_USB_Device_Library/Class/CDC/Inc
 -I$U/USB_DEVICE_MIDI/App -I$U/USB_DEVICE_MIDI/Target"
INC=$(echo $INC)   # one line: the compile command goes through eval
# delay.h's busy-wait inline functions are skipped; lxr_board.h declares the replacements
BOARD="-DDWT_STM32_DELAY_H"
# wasm only: Soundengine.c calls CodecInit(SYNTH_FS), AudioCodecManager.c defines
# int CodecInit() with no parameters. on arm the extra argument is ignored by the
# calling convention; wasm traps on the signature mismatch. dropping the argument
# at every call site is what the arm abi does. (see 0.notes, abi adaptations)
[ "$TARGET" = wasm ] && BOARD="$BOARD '-DCodecInit(...)=CodecInit()'"
objs=""; fail=0
python3 -I "$HERE/tools/firmware-sources.py" "$CCDB" "$U" > "$B/sources.tsv"
while IFS="$(printf '\t')" read -r src extra; do
  rel=${src#$U/}
  o="$B/obj/$(echo "$rel" | tr '/' '_' | sed 's/\.c$/.o/')"
  x=""
  case "$rel" in
    # main.c copies itcm code with a length from two linker symbols. the wasm build has
    # no itcm: make the length zero by resolving the end symbol to the start symbol.
    Core/Src/main.c) [ "$TARGET" = wasm ] && x="-D_itcm_text_end=_itcm_text_start";;
  esac
  [ "$TARGET" = wasm ] && extra=$(echo "$extra" | sed 's/-fno-thread-jumps//')   # gcc-only pass
  # TEST_CFLAGS: experiments only (e.g. strict ieee to check the board model). never for the real build.
  extra="$extra ${TEST_CFLAGS:-}"
  if eval "$CC $extra $TFLAGS $BOARD $x $INC -c \"$src\" -o \"$o\"" > "$o.log" 2>&1; then
    objs="$objs $o"
  else
    fail=$((fail+1)); printf 'FAIL %-60s %s errors\n' "$rel" "$(grep -c 'error:' "$o.log")"
  fi
done < "$B/sources.tsv"
echo "--- $TARGET: $(wc -l < "$B/sources.tsv") firmware files, $fail failed ---"
[ "$fail" = 0 ] || { cat "$B"/obj/*.log | grep 'error:' | sed 's/.*error: //' | sort | uniq -c | sort -rn | head -50; exit 1; }
[ "${ONLY_COMPILE:-0}" = 1 ] && exit 0

# ---- platform layer (virtual board, hal function bodies, sd card file) ----
PLAT="$HERE/platform/lxr_remap.c"
for f in "$HERE"/platform/*.c; do [ "$f" = "$HERE/platform/lxr_remap.c" ] || PLAT="$PLAT $f"; done
[ "$TARGET" = arm ] && PLAT="$PLAT $HERE/platform/arm/startup.c"
[ "$TARGET" = wasm ] && PLAT="$PLAT $HERE/shim-wasm/lxr_core_objects.c"
pobjs=""
for src in $PLAT; do
  o="$B/plat_$(basename "$src" .c).o"
  eval "$CC -DSTM32F765xx -DUSE_HAL_DRIVER -O3 -std=gnu11 -Wall -ffast-math -freciprocal-math $TFLAGS $BOARD $INC -c \"$src\" -o \"$o\""
  pobjs="$pobjs $o"
done

# ---- link ----
if [ "$TARGET" = wasm ]; then
  emcc -O3 $objs $pobjs -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sSTACK_SIZE=1MB -Wl,--error-limit=0 -o "$B/lxr.js" 2> "$B/link.log" || {
    grep -oE 'undefined symbol: [A-Za-z0-9_]+' "$B/link.log" | sort -u | sed 's/undefined symbol: //' > "$B/undefined.txt"
    echo "link failed: $(wc -l < "$B/undefined.txt") undefined symbols (build/$TARGET/undefined.txt)"; grep -v 'undefined symbol' "$B/link.log" | head -5; exit 1; }
  echo "linked build/wasm/lxr.js"
  # same objects for the browser page (worker, in-memory fs; the page attaches devices)
  emcc -O3 $objs $pobjs -sENVIRONMENT=worker -sMODULARIZE=1 -sEXPORT_NAME=LXRPLAIN -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 \
    -sEXPORTED_RUNTIME_METHODS=FS,callMain -sINITIAL_MEMORY=160MB -sEXIT_RUNTIME=1 -sSTACK_SIZE=1MB -o "$B/lxr-web.js"
  echo "linked build/wasm/lxr-web.js"
else
  $CC $TFLAGS --specs=rdimon.specs -T "$HERE/platform/arm/mps2-an500.ld" -Wl,--gc-sections $objs $pobjs -lm -o "$B/lxr.elf" 2> "$B/link.log" || {
    grep -oE "undefined reference to \\`[A-Za-z0-9_]+'" "$B/link.log" | sed "s/.*\\`//; s/'//" | sort -u > "$B/undefined.txt"
    echo "link failed: $(wc -l < "$B/undefined.txt") undefined symbols (build/$TARGET/undefined.txt)"; grep -v 'undefined reference' "$B/link.log" | head -5; exit 1; }
  "$TC/arm-none-eabi-size" "$B/lxr.elf"
  echo "linked build/arm/lxr.elf"
fi
