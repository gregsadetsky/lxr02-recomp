# lxr02-recomp

the erica synths / sonic potions LXR-02 firmware in the browser. alpha. erica's open source code
(unmodified, her compiler flags) built for cortex-m7, then translated instruction by
instruction to wasm. output is bit identical to the same build on qemu's cortex-m7.

- `upstream/` the firmware (submodule)
- `2.engine/` virtual board under the firmware, qemu reference runs
- `3.recomp/` arm elf to c to wasm
- `4.web/` the page

run: `GROOVE_DIR=/tmp/grooves uv run gunicorn app:app` (GROOVE_DIR: where saved grooves go, see .env.example)

gpl-3.0

## build

needs the arm gnu toolchain 14.3.rel1 (`ARM_TOOLCHAIN=<its bin dir>`), cmake, ninja,
emscripten, uv, mtools, qemu (tests). the blank card is made with macos `hdiutil`.

```
git clone --recursive https://github.com/gregsadetsky/lxr02-recomp && cd lxr02-recomp
cmake -S upstream/lxr-desktop-app -B build-fw/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DARM_TOOLCHAIN_DIR=$ARM_TOOLCHAIN
cmake --build build-fw/release              # erica's build: the source list and per file flags
2.engine/tools/mkcard.sh 2.engine/cards/blank-fat32-64m.img
2.engine/tools/mkcard-kit.sh 2.engine/cards/blank-fat32-64m.img 2.engine/cards/src/04-zippe.snd 2.engine/cards/kit-zippe.img
2.engine/tools/mkcard-project.sh 2.engine/cards/blank-fat32-64m.img 2.engine/cards/web.img 2.engine/cards/src/web/*.snd
2.engine/build.sh arm && 4.web/build.sh     # -> 4.web/site
```

tests: `uv run pytest tests` (server), `4.web/tests/` (save: js vs python, end to end), `2.engine/check.sh` (translation vs qemu golden hashes, `check.sh update` makes them),
`3.recomp/test/build.sh` (fp instructions, qemu vs translation), and per scenario
`cd 2.engine && ./run.sh qemu kit-zippe && ./run.sh recompw kit-zippe && ./compare.sh kit-zippe qemu recompw`
