OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x00001000;

    .text :
    {
        *(.text.startup)
        *(.text*)
        *(.rodata*)
    }

    . = ALIGN(8);

    .data :
    {
        *(.data*)
        *(.sdata*)
    }

    . = ALIGN(8);

    .bss (NOLOAD) :
    {
        *(.bss*)
        *(.sbss*)
        *(COMMON)
    }

    . = ALIGN(8);
    _heap_start = .;
    _end = .;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment*)
        *(.rela*)
        *(.debug*)
    }
}
