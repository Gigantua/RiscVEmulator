/*
 * A direct M-mode ECALL raises an environment-call trap. The CPU enters the
 * handler through the trap-frame page; the handler verifies cause 11
 * (ECALL-from-M), advances EPC past the ECALL, and returns via the gateway.
 *
 * Trap state lives in the hardware trap-frame page at 0x0F000000:
 *   +0x004  TRAP_VECTOR   handler entry PC
 *   +0x100  trap frame    word[0]=epc, word[1..31]=x1..x31,
 *                         word[32]=status, word[33]=tval, word[34]=cause
 * Trap return: put the frame address in a0 and jump to the resume gateway
 * at 0xFFFF0004 (the base ISA has no MRET).
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"        /* TRAP_VECTOR = handler */
        "ecall\n"               /* M-mode ECALL -> cause 11 */
        "li a0, 0\n"
        "j 3f\n"
        ".align 2\n"
        "1:\n"                  /* handler */
        "li t3, 0x0F000000\n"
        "lw t0, 0x188(t3)\n"    /* t0 = frame cause (word 34) */
        "li t1, 11\n"
        "bne t0, t1, 2f\n"
        "lw t0, 0x100(t3)\n"    /* t0 = frame epc (word 0) */
        "addi t0, t0, 4\n"
        "sw t0, 0x100(t3)\n"    /* epc += 4 */
        "li a0, 0x0F000100\n"   /* a0 = &frame (the trap frame) */
        "li t0, 0xFFFF0004\n"   /* trap return: jump to the resume gateway */
        "jr t0\n"
        "2:\n"
        "li a0, 1\n"
        "3:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        ".option pop\n"
        ::: "a0", "t0", "t1", "t3", "memory");
}
