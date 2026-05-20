/* factorial.c
 * Computes 20! recursively using unsigned long long (uint64_t on ILP32).
 * 20! = 2432902008176640000 = 0x21C3677C:82B40000
 *
 * Also computes a few smaller factorials for sanity checking.
 * Output: "N!=HI:LO\n" in hex, plus a decimal line for 20!.
 *
 * Needs softfloat.c linked for __udivdi3/__umoddi3 (used by decimal printer).
 */
#include "libc.h"

typedef unsigned long long u64;

/* printf doesn't support %llu, so keep a custom decimal printer */
static void put_u64_dec(u64 v)
{
    char buf[21];
    char *p = buf + 20;
    *p = '\0';
    if (v == 0) { printf("0"); return; }
    while (v > 0) { *--p = '0' + (char)(v % 10); v /= 10; }
    printf("%s", p);
}

/* Recursive factorial */
static u64 factorial(unsigned int n)
{
    if (n <= 1) return 1;
    return (u64)n * factorial(n - 1);
}

void _start(void)
{
    u64 f;

    /* Small factorials as sanity checks */
    f = factorial(1);                   /* 1 */
    printf("1!=%08X:%08X\n", (unsigned int)(f >> 32), (unsigned int)f);

    f = factorial(5);                   /* 120 = 0x00000000:00000078 */
    printf("5!=%08X:%08X\n", (unsigned int)(f >> 32), (unsigned int)f);

    f = factorial(10);                  /* 3628800 = 0x00000000:00375F00 */
    printf("10!=%08X:%08X\n", (unsigned int)(f >> 32), (unsigned int)f);

    f = factorial(13);                  /* 6227020800 = 0x00000001:7328CC00 */
    printf("13!=%08X:%08X\n", (unsigned int)(f >> 32), (unsigned int)f);

    /* The big one: 20! */
    f = factorial(20);                  /* 2432902008176640000 = 0x21C3677C:82B40000 */
    printf("20!=%08X:%08X\n", (unsigned int)(f >> 32), (unsigned int)f);

    /* Also print 20! in decimal */
    printf("20!dec="); put_u64_dec(f); printf("\n");

    exit(0);
}
