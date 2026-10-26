#!/bin/sh
# build the fp vector test for cortex-m7 (qemu mps2-an500), run it on qemu, translate it,
# run the translation, compare the two fpvec.bin files. usage: test/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
TC=$ROOT/tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin
B=$HERE/build; mkdir -p "$B/qemu" "$B/recomp"
"$TC/arm-none-eabi-gcc" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -O2 -std=gnu11 -Wall \
  --specs=nano.specs --specs=rdimon.specs -T "$ROOT/2.engine/platform/arm/mps2-an500.ld" -Wl,-e,tv_reset \
  "$HERE/fpvec.c" -o "$B/fpvec.elf"
( cd "$B/qemu" && timeout 600 qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic \
    -semihosting-config enable=on,target=native -kernel ../fpvec.elf )
"$HERE/../build-native.sh" "$B/fpvec.elf" "$B/gen" > /dev/null
( cd "$B/recomp" && "$B/gen/lxr-native" )
ls -la "$B/qemu/fpvec.bin" "$B/recomp/fpvec.bin"
uv run --no-project python "$HERE/diff.py" "$B/qemu/fpvec.bin" "$B/recomp/fpvec.bin" "$HERE/fpvec.c"
