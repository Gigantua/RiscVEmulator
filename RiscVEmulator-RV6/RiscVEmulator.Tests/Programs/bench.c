/* bench.c — CPU-bound integer kernel for measuring emulator throughput (MIPS).
 *
 * Pure RV32I: xorshift over a 64-word array — ALU ops, loads, stores and
 * branches in a tight nested loop, no mul/div. Sized to run a few seconds
 * on the interpreter so a stopwatch gives a stable steps/second figure.
 */

#include "libc.h"

#define N      64
#define ITERS  400000u

void _start(void)
{
    unsigned int state[N];
    unsigned int seed = 0x9E3779B9u;
    for (int i = 0; i < N; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        state[i] = seed;
    }

    unsigned int acc = 0x12345678u;
    for (unsigned int it = 0; it < ITERS; it++) {
        for (int i = 0; i < N; i++) {
            unsigned int x = state[i];
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            acc += x;
            acc = (acc << 7) | (acc >> 25);
            state[i] = x ^ acc;
        }
    }

    printf("result=%u\n", acc);
    exit(0);
}
