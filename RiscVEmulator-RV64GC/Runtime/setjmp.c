/* setjmp.c — RV64 setjmp/longjmp for bare-metal emulator.
 *
 * jmp_buf layout (14 × uint64_t — 112 bytes):
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
 *
 * Callee-saved FP registers (fs0-fs11) are not preserved — no bare-metal
 * example relies on a live FP value surviving a longjmp.
 */

__attribute__((naked)) int setjmp(void *env)
{
    __asm__(
        "sd  ra,   0(a0)\n"
        "sd  sp,   8(a0)\n"
        "sd  s0,  16(a0)\n"
        "sd  s1,  24(a0)\n"
        "sd  s2,  32(a0)\n"
        "sd  s3,  40(a0)\n"
        "sd  s4,  48(a0)\n"
        "sd  s5,  56(a0)\n"
        "sd  s6,  64(a0)\n"
        "sd  s7,  72(a0)\n"
        "sd  s8,  80(a0)\n"
        "sd  s9,  88(a0)\n"
        "sd s10,  96(a0)\n"
        "sd s11, 104(a0)\n"
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
        "ld  ra,   0(t1)\n"
        "ld  sp,   8(t1)\n"
        "ld  s0,  16(t1)\n"
        "ld  s1,  24(t1)\n"
        "ld  s2,  32(t1)\n"
        "ld  s3,  40(t1)\n"
        "ld  s4,  48(t1)\n"
        "ld  s5,  56(t1)\n"
        "ld  s6,  64(t1)\n"
        "ld  s7,  72(t1)\n"
        "ld  s8,  80(t1)\n"
        "ld  s9,  88(t1)\n"
        "ld s10,  96(t1)\n"
        "ld s11, 104(t1)\n"
        "ret\n"
    );
}
