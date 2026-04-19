/* timer_test.c — Tests the CLINT timer peripheral.
 * Reads mtime, busy-waits, reads again, verifies it advanced.
 * Compile: clang -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -O1
 */

#include "libc.h"

#define MTIME_LO   (*(volatile unsigned int *)0x0200BFF8)
#define MTIME_HI   (*(volatile unsigned int *)0x0200BFFC)

static void check(const char *label, int pass)
{
    printf("%s: %s\n", label, pass ? "OK" : "FAIL");
}

void _start(void)
{
    /* Read initial mtime */
    unsigned int t0_lo = MTIME_LO;
    unsigned int t0_hi = MTIME_HI;

    /* Busy-wait: do some work to advance mtime */
    volatile unsigned int dummy = 0;
    for (int i = 0; i < 1000; i++)
        dummy += i;

    /* Read mtime again */
    unsigned int t1_lo = MTIME_LO;
    unsigned int t1_hi = MTIME_HI;

    /* Compute elapsed (assume no overflow in low word for this small test) */
    unsigned int elapsed = t1_lo - t0_lo;

    printf("timer_test\n");
    printf("t0=%u\n", t0_lo);
    printf("t1=%u\n", t1_lo);
    printf("elapsed=%u\n", elapsed);

    check("mtime_advances", elapsed > 0);
    check("mtime_reasonable", elapsed > 100);  /* loop should take >100 instructions */
    check("mtime_hi_zero", t0_hi == 0 && t1_hi == 0);  /* early in execution */

    exit(0);
}
