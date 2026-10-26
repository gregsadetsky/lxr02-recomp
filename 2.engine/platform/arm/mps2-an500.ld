/* qemu mps2-an500 (cortex-m7): 4 mb ssram at 0. everything runs from there.
 * the firmware's itcm/dtcm sections are placed in that ram; main() copies
 * _itcm_text_end - _itcm_text_start bytes, which is 0 here (code already in place). */
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M }
ENTRY(lxr_reset)
SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > RAM
  .text : {
    *(.text*) _itcm_text_start = .; _itcm_text_end = .; _itcm_data = .; *(.itcm_text*)
    *(.rodata*) *(.glue_7) *(.glue_7t) *(.eh_frame) KEEP(*(.init)) KEEP(*(.fini))
  } > RAM
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > RAM
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > RAM
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > RAM
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } > RAM
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } > RAM
  .data : { . = ALIGN(8); *(.data*) *(.dtcm) *(.dtcm*) . = ALIGN(8); } > RAM
  .bss (NOLOAD) : { . = ALIGN(8); __bss_start__ = .; *(.bss*) *(.dtcmz) *(.dtcmz*) *(COMMON) . = ALIGN(8); __bss_end__ = .; } > RAM
  .heap (NOLOAD) : { . = ALIGN(8); __end__ = .; end = .; . += 1M; __heap_limit = .; __HeapLimit = .; } > RAM
  .stack (NOLOAD) : { . = ALIGN(8); __StackLimit = .; . += 256K; . = ALIGN(8); __StackTop = .; __stack = .; } > RAM
}
