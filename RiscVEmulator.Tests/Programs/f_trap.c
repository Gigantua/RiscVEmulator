/*
 * Verifies RV32F opcodes are unsupported. The test installs an M-mode trap
 * handler by writing TRAP_VECTOR in the hardware trap-frame page
 * (0x0F000000 + 0x04), executes one raw F-extension instruction, and exits 0
 * only if the illegal-instruction trap is taken.
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"        /* TRAP_VECTOR = handler */
        ".word 0x00000053\n"    /* fadd.s f0, f0, f0, rne */
        "li a0, 1\n"
        "j 2f\n"
        ".align 2\n"
        "1:\n"
        "li a0, 0\n"
        "2:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        ".option pop\n"
        ::: "a0", "t0", "t3", "memory");
}
