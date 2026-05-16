/* double_basics.c
 * Tests scalar IEEE 754 double-precision operations.
 *
 * All values are exactly representable in float64.  Compiled with
 * -march=rv32i -nostdlib; arithmetic provided by softfloat.c.
 *
 * Output: one result per line, printed as a signed integer
 * (cast from double — exact since all test results are whole numbers
 * or converted precisely via __fixdfsi).
 */
#include "libc.h"

void _start(void)
{
    volatile double a, b, r;

    /* ── Arithmetic ─────────────────────────────────────────── */
    a = 1.5; b = 2.5;
    r = a + b;                        /* 4.0 */
    printf("add=%d\n", (int)r);

    a = 10.0; b = 3.0;
    r = a - b;                        /* 7.0 */
    printf("sub=%d\n", (int)r);

    a = 3.0; b = 4.0;
    r = a * b;                        /* 12.0 */
    printf("mul=%d\n", (int)r);

    a = 12.0; b = 4.0;
    r = a / b;                        /* 3.0 */
    printf("div=%d\n", (int)r);

    a = 6.0; b = 2.0;
    r = a / b;                        /* 3.0 */
    printf("div2=%d\n", (int)r);

    a = 9.0; b = 3.0;
    r = a / b;                        /* 3.0 */
    printf("div3=%d\n", (int)r);

    /* ── Negative numbers ────────────────────────────────────── */
    a = -5.0; b = 3.0;
    r = a + b;                        /* -2.0 */
    printf("neg_add=%d\n", (int)r);

    a = -6.0; b = -2.0;
    r = a * b;                        /* 12.0 */
    printf("neg_mul=%d\n", (int)r);

    /* ── Conversions ─────────────────────────────────────────── */
    volatile int ix = 42;
    r = (double)ix;                   /* 42.0 */
    printf("i2d=%d\n", (int)r);

    a = 7.5;                          /* truncates to 7 */
    printf("d2i=%d\n", (int)a);

    a = -3.0;
    printf("neg_d2i=%d\n", (int)a);

    /* ── Comparisons ─────────────────────────────────────────── */
    a = 3.0; b = 4.0;
    printf("lt=%d\n", (a < b) ? 1 : 0);  /* 1 */
    printf("gt=%d\n", (a > b) ? 1 : 0);  /* 0 */

    a = 4.0; b = 4.0;
    printf("eq=%d\n", (a == b) ? 1 : 0); /* 1 */
    printf("ge=%d\n", (a >= b) ? 1 : 0); /* 1 */
    printf("le=%d\n", (a <= b) ? 1 : 0); /* 1 */

    a = 5.0; b = 4.0;
    printf("ne=%d\n", (a != b) ? 1 : 0); /* 1 */

    exit(0);
}
