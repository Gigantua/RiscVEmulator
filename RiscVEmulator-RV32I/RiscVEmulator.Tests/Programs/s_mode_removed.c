/*
 * S-mode, WFI, PMP, machine-ID, ENVCFG, SEED — and every CSR instruction —
 * trap as illegal instructions, while the M/U trap path keeps working.
 * Eight deliberately-illegal opcodes are laid out back to back as raw .word
 * encodings; the handler verifies each raises cause 2, counts it, and
 * resumes at the next instruction (epc += 4). The test passes only if all
 * eight trapped. (FENCE/FENCE.I are valid RV32I and retire as NOP, so they
 * are not part of this set.)
 *
 * Trap state lives in the hardware trap-frame page at 0x0F000000:
 *   +0x004  TRAP_VECTOR   handler entry PC
 *   +0x010  scratch       trap counter (test-private)
 *   +0x100  landing pad   pt_regs layout: word[0]=epc, word[34]=cause, ...
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"        /* TRAP_VECTOR = handler */
        "sw zero, 0x10(t3)\n"   /* trap counter = 0 */
        ".word 0x10200073\n"    /* sret           */
        ".word 0x10529073\n"    /* csrw stvec, t0 */
        ".word 0x30229073\n"    /* csrw medeleg, t0 */
        ".word 0x10500073\n"    /* wfi            */
        ".word 0xf11022f3\n"    /* csrr t0, mvendorid */
        ".word 0x3a029073\n"    /* csrw pmpcfg0, t0 */
        ".word 0x30a2a073\n"    /* csrs menvcfg, t0 */
        ".word 0x015055f3\n"    /* csrrwi a1, seed, 0 */
        "lw t1, 0x10(t3)\n"     /* trap counter */
        "li t2, 8\n"
        "bne t1, t2, 4f\n"
        "li a0, 0\n"
        "j 5f\n"
        "4:\n"
        "li a0, 1\n"
        "5:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        "j 6f\n"
        ".align 2\n"
        "1:\n"                  /* handler */
        "li t4, 0x0F000000\n"
        "lw t5, 0x188(t4)\n"    /* t5 = frame cause (word 34) */
        "li t6, 2\n"
        "bne t5, t6, 9f\n"      /* only count illegal-instruction traps */
        "lw t5, 0x10(t4)\n"
        "addi t5, t5, 1\n"
        "sw t5, 0x10(t4)\n"     /* trap counter += 1 */
        "9:\n"
        "lw t5, 0x100(t4)\n"    /* t5 = frame epc (word 0) */
        "addi t5, t5, 4\n"
        "sw t5, 0x100(t4)\n"    /* epc += 4 -> skip the faulting opcode */
        "li a0, 0x0F000100\n"   /* a0 = &frame */
        "li t0, 0xFFFF0004\n"   /* trap return: jump to the resume gateway */
        "jr t0\n"
        "6:\n"
        ".option pop\n"
        ::: "a0", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");
}
