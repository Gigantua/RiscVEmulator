/* float_basics.c
 * Tests scalar IEEE 754 single-precision (float) operations.
 *
 * All values are exactly representable in float32, so no rounding occurs
 * and the results are deterministic.  Compiled with -march=rv32im -nostdlib;
 * float arithmetic is provided by softfloat.c (compiler-rt ABI).
 *
 * Output: one result per line, printed as a signed integer.
 */
#include "libc.h"

void _start(void)
{
    volatile float a, b, r;

    /* ── Arithmetic ─────────────────────────────────────────── */
    a = 1.5f; b = 2.5f;
    r = a + b;                       /* 4.0 */
    printf("add=%d\n", (int)r);

    a = 10.0f; b = 3.0f;
    r = a - b;                       /* 7.0 */
    printf("sub=%d\n", (int)r);

    a = 3.0f; b = 4.0f;
    r = a * b;                       /* 12.0 */
    printf("mul=%d\n", (int)r);

    a = 12.0f; b = 4.0f;
    r = a / b;                       /* 3.0 */
    printf("div=%d\n", (int)r);

    a = 6.0f; b = 2.0f;
    r = a / b;                       /* 3.0 */
    printf("div2=%d\n", (int)r);

    a = 9.0f; b = 3.0f;
    r = a / b;                       /* 3.0 */
    printf("div3=%d\n", (int)r);

    /* ── Negative numbers ────────────────────────────────────── */
    a = -5.0f; b = 3.0f;
    r = a + b;                       /* -2.0 */
    printf("neg_add=%d\n", (int)r);

    a = -6.0f; b = -2.0f;
    r = a * b;                       /* 12.0 */
    printf("neg_mul=%d\n", (int)r);

    /* ── Conversions ─────────────────────────────────────────── */
    volatile int ix = 42;
    r = (float)ix;                   /* 42.0 */
    printf("i2f=%d\n", (int)r);

    a = 7.5f;                        /* truncates to 7 */
    printf("f2i=%d\n", (int)a);

    a = -3.0f;                       /* negative float to int */
    printf("neg_f2i=%d\n", (int)a);

    /* ── Comparisons ─────────────────────────────────────────── */
    a = 3.0f; b = 4.0f;
    printf("lt=%d\n", (a < b) ? 1 : 0);  /* 1 */
    printf("gt=%d\n", (a > b) ? 1 : 0);  /* 0 */

    a = 4.0f; b = 4.0f;
    printf("eq=%d\n", (a == b) ? 1 : 0); /* 1 */
    printf("ge=%d\n", (a >= b) ? 1 : 0); /* 1 */
    printf("le=%d\n", (a <= b) ? 1 : 0); /* 1 */

    a = 5.0f; b = 4.0f;
    printf("ne=%d\n", (a != b) ? 1 : 0); /* 1 */

    exit(0);
}
