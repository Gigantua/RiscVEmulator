/* setjmp.c — RV32I setjmp/longjmp for bare-metal emulator.
 *
 * jmp_buf layout (14 × uint32_t):
 *   [0]  ra   (x1)
 *   [1]  sp   (x2)
 *   [2]  s0   (x8)
 *   [3]  s1   (x9)
 *   [4]  s2  (x18)
 *   [5]  s3  (x19)
 *   [6]  s4  (x20)
 *   [7]  s5  (x21)
 *   [8]  s6  (x22)
 *   [9]  s7  (x23)
 *  [10]  s8  (x24)
 *  [11]  s9  (x25)
 *  [12]  s10 (x26)
 *  [13]  s11 (x27)
 */

__attribute__((naked)) int setjmp(void *env)
{
    __asm__(
        "sw  ra,  0(a0)\n"
        "sw  sp,  4(a0)\n"
        "sw  s0,  8(a0)\n"
        "sw  s1, 12(a0)\n"
        "sw  s2, 16(a0)\n"
        "sw  s3, 20(a0)\n"
        "sw  s4, 24(a0)\n"
        "sw  s5, 28(a0)\n"
        "sw  s6, 32(a0)\n"
        "sw  s7, 36(a0)\n"
        "sw  s8, 40(a0)\n"
        "sw  s9, 44(a0)\n"
        "sw s10, 48(a0)\n"
        "sw s11, 52(a0)\n"
        "li  a0, 0\n"
        "ret\n"
    );
}

__attribute__((naked, noreturn)) void longjmp(void *env, int val)
{
    __asm__(
        "mv  t1,  a0\n"          /* t1 = env */
        "seqz t0, a1\n"          /* t0 = (val == 0) */
        "add  a0, a1, t0\n"      /* a0 = val ? val : 1 */
        "lw  ra,  0(t1)\n"
        "lw  sp,  4(t1)\n"
        "lw  s0,  8(t1)\n"
        "lw  s1, 12(t1)\n"
        "lw  s2, 16(t1)\n"
        "lw  s3, 20(t1)\n"
        "lw  s4, 24(t1)\n"
        "lw  s5, 28(t1)\n"
        "lw  s6, 32(t1)\n"
        "lw  s7, 36(t1)\n"
        "lw  s8, 40(t1)\n"
        "lw  s9, 44(t1)\n"
        "lw s10, 48(t1)\n"
        "lw s11, 52(t1)\n"
        "ret\n"
    );
}
