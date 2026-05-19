/* Flat-binary link layout for bare-metal RV64GC test programs.
 * Loaded verbatim at 0x80000000 and entered at _start. .bss is not in the
 * image — it lands in the host's zero-initialised RAM. */
ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text   : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) *(.srodata*) }
    .data   : { *(.data*) *(.sdata*) }
    .bss    : { *(.bss*) *(.sbss*) *(COMMON) }
}
