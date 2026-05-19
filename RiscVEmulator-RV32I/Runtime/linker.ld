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

    . = ALIGN(4);

    .data :
    {
        *(.data*)
        *(.sdata*)
    }

    . = ALIGN(4);

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
