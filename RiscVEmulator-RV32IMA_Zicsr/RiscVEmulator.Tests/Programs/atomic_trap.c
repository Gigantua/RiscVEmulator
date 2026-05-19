/*
 * Verifies RV32A opcodes are unsupported. The test installs an M-mode trap
 * vector, executes one raw A-extension instruction, and exits 0 only if the
 * illegal-instruction trap is taken.
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        ".word 0x30529073\n"    /* csrw mtvec, t0 */
        ".word 0x100020af\n"    /* lr.w x1, (x0) */
        "li a0, 1\n"
        "j 2f\n"
        ".align 2\n"
        "1:\n"
        "li a0, 0\n"
        "2:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        ".option pop\n"
        ::: "a0", "t0", "memory");
}
