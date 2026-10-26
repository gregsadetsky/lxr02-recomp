#!/usr/bin/env python3
"""list the firmware translation units from erica's own cmake build
(build-fw/release/compile_commands.json) with their per-file extra flags, minus
the ones the virtual board replaces. prints "<source>\t<extra flags>" lines.

replaced (hardware layer only, see CLAUDE.md):
  Drivers/STM32F7xx_HAL_Driver/Src/*  st hal function bodies -> platform/hal*.c
  Core/Startup/*.s                    reset/vector code     -> host startup
  Core/Src/syscalls.c, sysmem.c       newlib glue           -> host libc
  Core/Src/delay.c                    dwt cycle counter init (inline asm nop) -> board
everything else is compiled unmodified, including main.c and stm32f7xx_it.c.
usage: firmware-sources.py <compile_commands.json> <upstream dir>"""
import json, shlex, sys, os
cc, up = sys.argv[1], os.path.realpath(sys.argv[2])
REPLACED_DIRS = ["Drivers/STM32F7xx_HAL_Driver/Src/", "Core/Startup/"]
REPLACED_FILES = ["Core/Src/syscalls.c", "Core/Src/sysmem.c", "Core/Src/delay.c"]
# flags that only exist for arm gcc or that the wasm/arm host builds set themselves
DROP_PREFIX = ("-I", "-mcpu", "-mthumb", "-mfpu", "-mfloat-abi", "--specs", "-mfp16-format", "-o", "-c", "-MD", "-MT", "-MF", "-g")
COMMON = None
for e in json.load(open(cc)):
    f = os.path.realpath(e["file"])
    rel = os.path.relpath(f, up)
    if any(rel.startswith(d) for d in REPLACED_DIRS) or rel in REPLACED_FILES: continue
    toks = shlex.split(e["command"])[1:]
    out, skip = [], False
    for t in toks:
        if skip: skip = False; continue
        if t in ("-o", "-MF", "-MT"): skip = True; continue
        if t.startswith(DROP_PREFIX) or t == f or t.endswith((".c", ".s")): continue
        out.append(t)
    print("%s\t%s" % (f, " ".join(shlex.quote(x) for x in out)))
