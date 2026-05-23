/*
 * Exercises the hardware trap frame on the path a kernel boot depends on:
 * enable the CLINT machine-timer interrupt, take it asynchronously through
 * the trap-frame page while ordinary code runs, handle it, and return via
 * the resume gateway so the interrupted code resumes.
 *
 * This is the async-interrupt counterpart to nested_trap.c (synchronous):
 * it covers interrupt arbitration (IE_FLAG / IE_MASK gating, the MTIP pin),
 * do_trap building a pt_regs-layout frame, and trap return applying mret
 * semantics to the status word (MPIE -> MIE re-enables interrupts).
 *
 * Passes only if the timer interrupt actually fired (cause 0x80000007) and
 * the interrupted spin loop resumed and observed the handler's flag.
 *
 * Trap-frame page 0x0F000000: IE_FLAG +0, TRAP_VECTOR +4, IE_MASK +8,
 *   landing pad +0x100 (pt_regs layout; cause = word 34 = +0x188).
 * Test scratch: +0x10 handled flag, +0x14 observed cause.
 * CLINT mtimecmp at 0x02004000.
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"           /* TRAP_VECTOR = handler */
        "sw zero, 0x10(t3)\n"      /* handled flag = 0 */
        "li t0, 0x80\n"
        "sw t0, 8(t3)\n"           /* IE_MASK = MTIP (bit 7) */
        /* arm the CLINT timer: mtimecmp = 0 -> MTIP asserts immediately */
        "li t1, 0x02004000\n"
        "sw zero, 0(t1)\n"
        "sw zero, 4(t1)\n"
        /* enable interrupts: IE_FLAG = MIE (bit 3) */
        "li t0, 8\n"
        "sw t0, 0(t3)\n"
        /* spin (bounded) until the handler reports it ran */
        "li t1, 200000\n"
        "2:\n"
        "lw t0, 0x10(t3)\n"
        "bnez t0, 3f\n"
        "addi t1, t1, -1\n"
        "bnez t1, 2b\n"
        "3:\n"
        /* verify: handler ran AND the cause was a machine-timer interrupt */
        "lw t0, 0x10(t3)\n"
        "beqz t0, 4f\n"
        "lw t0, 0x14(t3)\n"
        "li t1, 0x80000007\n"
        "bne t0, t1, 4f\n"
        "li a0, 0\n"
        "j 5f\n"
        "4:\n"
        "li a0, 1\n"
        "5:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        "j 6f\n"

        ".align 2\n"
        "1:\n"                     /* machine-timer interrupt handler */
        "li t4, 0x0F000000\n"
        "lw t5, 0x188(t4)\n"       /* cause = frame word 34 */
        "sw t5, 0x14(t4)\n"        /* record the observed cause */
        /* disarm the timer: mtimecmp = max -> MTIP deasserts */
        "li t6, 0x02004000\n"
        "li t5, -1\n"
        "sw t5, 0(t6)\n"
        "sw t5, 4(t6)\n"
        "li t5, 1\n"
        "sw t5, 0x10(t4)\n"        /* handled flag = 1 */
        /* return: the landing pad IS the pt_regs-layout frame */
        "li a0, 0x0F000100\n"
        "li t0, 0xFFFF0004\n"      /* trap return: jump to the resume gateway */
        /* RISC5: `jr t0` would emit a JALR; the CPU has removed case 0x67.
         * Use the tail-call sub-mode of JAL: `jal t0, 6` → PC = x[t0].
         * Encoded as: opcode=0x6F, rd=t0(5), imm=6 → 0x006002ef. */
        ".4byte 0x006002ef\n"
        "6:\n"
        ".option pop\n"
        ::: "a0", "t0", "t1", "t3", "t4", "t5", "t6", "memory");
}
