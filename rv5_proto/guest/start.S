.section .text._start
.globl _start
_start:
    # sp = 0x0FFFF0 (top of 1MB minus 16 bytes, 16-byte aligned)
    lui  sp, 0x100
    addi sp, sp, -16
    # gp / tp unused — leave zero
    call main
    # main returned: signal halt via EBREAK
1:  ebreak
    j 1b
