OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    /* RISC5 reduction: a small constant pool lives at low memory
     * (VMA 0..0x3F) so LW with a 12-bit positive offset from x0
     * reaches every entry. The LLVM pass replaces matching ADDI
     * immediates with LW from these slots — see RISCVReduceToFiveOps
     * `expandADDI` and Runtime/runtime.c `RVEMU_CONST_POOL`. */
    . = 0x00000000;

    .const_pool : ALIGN(4)
    {
        KEEP(*(.const_pool))
    }

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
