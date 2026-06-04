/*
 * Direct ECALL remains part of the guest ABI. This drops to U-mode and
 * executes an architectural ECALL, then verifies the CPU raises cause 8.
 *
 * Trap state lives in the hardware trap-frame page at 0x0F000000. To enter
 * U-mode the test crafts a trap frame in the landing pad — word[0]=epc set
 * to the U-mode entry, word[32]=status = 0 (status image: PP clear -> U,
 * PIE clear -> interrupts off) — points a0 at it, and jumps to the resume
 * gateway at 0xFFFF0004 (the base ISA has no MRET).
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"        /* TRAP_VECTOR = handler */
        "la t0, 2f\n"
        "sw t0, 0x100(t3)\n"    /* frame word[0] epc = U-mode entry */
        "sw zero, 0x180(t3)\n"  /* frame word[32] status = 0 (IE off, priv U) */
        "li a0, 0x0F000100\n"   /* a0 = &frame */
        "li t0, 0xFFFF0004\n"   /* trap return: jump to the resume gateway */
        "jr t0\n"               /* -> U-mode at 2f */
        "2:\n"
        "ecall\n"               /* U-mode ECALL -> cause 8 */
        "li a0, 2\n"
        "j 3f\n"
        ".align 2\n"
        "1:\n"                  /* handler */
        "li t3, 0x0F000000\n"
        "lw t0, 0x188(t3)\n"    /* t0 = frame cause (word 34) */
        "li t1, 8\n"
        "bne t0, t1, 4f\n"
        "li a0, 0\n"
        "j 3f\n"
        "4:\n"
        "li a0, 1\n"
        "3:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        ".option pop\n"
        ::: "a0", "t0", "t1", "t3", "memory");
}
