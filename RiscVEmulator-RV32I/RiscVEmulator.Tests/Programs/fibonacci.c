/* fibonacci.c
 * Compute and print fib(1)..fib(20).
 *
 * Exercises: recursive function calls, SW/LW of return address (ra) and
 * saved registers on the stack, JALR returns, SUB (n-1, n-2), BEQ base case.
 */

#include "libc.h"

/* Intentionally recursive to stress the stack. */
static unsigned int fib(unsigned int n)
{
    if (n <= 1u) return n;
    return fib(n - 1u) + fib(n - 2u);
}

void _start(void)
{
    for (unsigned int i = 1u; i <= 20u; i++) {
        printf("fib(%u)=%u\n", i, fib(i));
    }

    exit(0);
}
