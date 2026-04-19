/* primes.c
 * Prints the first 100 prime numbers.
 *
 * Compile:
 *   clang --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 \
 *         -nostdlib -nostartfiles -O1 -fuse-ld=lld \
 *         -Wl,-Tlinker.ld primes.c -o primes.elf
 *
 * Exit is via Linux ecall 93 (exit), which the emulator detects and
 * uses to terminate the simulation cleanly.
 */

#include "libc.h"

static int is_prime(unsigned int n)
{
    if (n < 2u) return 0;
    if (n == 2u) return 1;
    if ((n & 1u) == 0u) return 0;           /* even */
    for (unsigned int i = 3u; i * i <= n; i += 2u) {
        if (n % i == 0u) return 0;
    }
    return 1;
}

void _start(void)
{
    unsigned int count = 0u;
    unsigned int num   = 2u;

    printf("First 100 prime numbers:\n");

    while (count < 100u) {
        if (is_prime(num)) {
            printf("%u", num);
            count++;
            if (count % 10u == 0u)
                printf("\n");
            else
                printf("  ");
        }
        num++;
    }

    printf("\nDone.\n");

    exit(0);
}
