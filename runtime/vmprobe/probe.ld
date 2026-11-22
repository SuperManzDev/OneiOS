/* runtime/vmprobe: one flat image from offset 0, the Linux Image header first, everything in the file (no .bss). */
ENTRY(_head)
SECTIONS {
  . = 0;
  .text : { *(.text.head) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : ALIGN(16) { *(.data*) }
  /DISCARD/ : { *(.bss*) *(COMMON) *(.comment) *(.note*) }
}
