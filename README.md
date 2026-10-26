# lxr02-recomp

the erica synths / sonic potions lxr-02 firmware in the browser. erica's open source code
(unmodified, her compiler flags) built for cortex-m7, then translated instruction by
instruction to wasm. output is bit identical to the same build on qemu's cortex-m7.

- `upstream/` the firmware (submodule)
- `2.engine/` virtual board under the firmware, qemu reference runs
- `3.recomp/` arm elf to c to wasm
- `4.web/` the page

run: `uv run gunicorn app:app`

gpl-3.0
