/*
 * Regression test for the trap-frame re-entrancy hazard ("Window C").
 *
 * On a trap the CPU spills the register file into a single fixed landing pad
 * at 0x0F000100. If a handler took a second trap and then returned through
 * that same shared pad, the nested trap's spill would clobber the outer
 * context. The fix is asymmetric frames: entry always lands in the fixed
 * pad, but the handler copies it into a private per-invocation frame and the
 * resume gateway reloads from a handler-supplied pointer (a0).
 *
 * This test forces the hazard deterministically with a nested *synchronous*
 * trap (an ECALL inside the handler — same landing-pad mechanism a nested
 * timer interrupt would hit):
 *
 *   user: sets a canary in s1, executes ECALL
 *   handler invocation 1 (depth 1): copies the landing pad into frameA,
 *       then executes ECALL itself -> a nested trap reuses the landing pad
 *   handler invocation 2 (depth 2): copies the (now clobbered) landing pad
 *       into frameB and returns through frameB
 *   handler invocation 1 resumes, returns to the user through frameA
 *
 * The test passes only if the user's s1 canary survived the whole chain —
 * i.e. frameA was never corrupted by the nested trap — and both handler
 * invocations ran (depth == 2).
 *
 * Trap-frame page scratch (test-private, inside the 4 KB page):
 *   +0x010  depth counter
 *   +0x200  frameA (36 words)   +0x300  frameB (36 words)
 */
void _start(void)
{
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "li t3, 0x0F000000\n"
        "la t0, 1f\n"
        "sw t0, 4(t3)\n"        /* TRAP_VECTOR = handler */
        "sw zero, 0x10(t3)\n"   /* depth = 0 */
        "li s1, 0xCAFEBABE\n"   /* canary the user must keep across the trap */
        "ecall\n"               /* first trap */
        /* ---- resumes here once the outer handler returns ---- */
        "li t0, 0xCAFEBABE\n"
        "bne s1, t0, 8f\n"      /* canary survived the nested trap? */
        "li t3, 0x0F000000\n"
        "lw t0, 0x10(t3)\n"
        "li t1, 2\n"
        "bne t0, t1, 8f\n"      /* both handler invocations ran? */
        "li a0, 0\n"
        "j 9f\n"
        "8:\n"
        "li a0, 1\n"
        "9:\n"
        "li t0, 0x40000000\n"
        "sw a0, 0(t0)\n"
        "j 7f\n"

        ".align 2\n"
        "1:\n"                  /* ---- trap handler ---- */
        "li t0, 0x0F000000\n"
        "lw t1, 0x10(t0)\n"
        "addi t1, t1, 1\n"
        "sw t1, 0x10(t0)\n"     /* depth += 1 */
        "li t2, 1\n"
        /* `beq t1, t2, 3f`  ≡  `bne t1, t2, 5f ; j 3f ; 5:` */
        "bne t1, t2, 5f\n"      /* depth != 1 → skip into the nested path */
        "j 3f\n"                /* depth == 1 → jump to outer */
        "5:\n"
        /* ---- nested (depth >= 2): copy landing pad -> frameB, return ---- */
        "li t3, 0x0F000100\n"
        "li t4, 0x0F000300\n"
        "li t5, 0\n"
        "2:\n"
        "lw t6, 0(t3)\n"
        "sw t6, 0(t4)\n"
        "addi t3, t3, 4\n"
        "addi t4, t4, 4\n"
        "addi t5, t5, 1\n"
        "li t2, 36\n"
        "bne t5, t2, 2b\n"      /* t5 counts 0..36 by 1 — bne == blt here */
        "li t4, 0x0F000300\n"
        "lw t6, 0(t4)\n"
        "addi t6, t6, 4\n"
        "sw t6, 0(t4)\n"        /* frameB epc (word 0) += 4 -> skip the handler's ECALL */
        "li a0, 0x0F000300\n"   /* a0 = &frameB */
        "li t0, 0xFFFF0004\n"   /* trap return: jump to the resume gateway */
        "jr t0\n"
        "3:\n"                  /* ---- outer (depth == 1) ---- */
        "li t3, 0x0F000100\n"
        "li t4, 0x0F000200\n"
        "li t5, 0\n"
        "4:\n"
        "lw t6, 0(t3)\n"
        "sw t6, 0(t4)\n"
        "addi t3, t3, 4\n"
        "addi t4, t4, 4\n"
        "addi t5, t5, 1\n"
        "li t2, 36\n"
        "bne t5, t2, 4b\n"      /* same counter-loop trick: bne == blt */
        "ecall\n"               /* nested trap: reuses the shared landing pad */
        /* ---- nested handler returned here; now return to the user ---- */
        "li t4, 0x0F000200\n"
        "lw t6, 0(t4)\n"
        "addi t6, t6, 4\n"
        "sw t6, 0(t4)\n"        /* frameA epc (word 0) += 4 -> skip the user's ECALL */
        "li a0, 0x0F000200\n"   /* a0 = &frameA (private; untouched by nesting) */
        "li t0, 0xFFFF0004\n"   /* trap return: jump to the resume gateway */
        "jr t0\n"
        "7:\n"
        ".option pop\n"
        ::: "a0", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s1", "memory");
}
