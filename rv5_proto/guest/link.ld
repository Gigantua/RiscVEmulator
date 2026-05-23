ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x00080000
}

SECTIONS {
    .text 0x0 : {
        KEEP(*(.text._start))
        *(.text*)
    } > RAM

    .rodata : ALIGN(4) {
        *(.rodata*)
        *(.srodata*)
    } > RAM

    .data : ALIGN(4) {
        *(.data*)
        *(.sdata*)
    } > RAM

    .bss : ALIGN(4) {
        _bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.riscv.attributes)
        *(.eh_frame*)
    }
}
