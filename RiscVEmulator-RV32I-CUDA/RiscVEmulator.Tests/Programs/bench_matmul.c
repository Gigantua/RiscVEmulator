/* bench_matmul.c — Integer matrix multiply.
 *
 * Stresses __mulsi3 (M extension is removed, so every guest `mul` becomes a
 * libcall into the bare-metal runtime). Cubic work + lots of indexed loads
 * and stores. Sized to take a few seconds on the interpreter.
 */

#include "libc.h"

#define N      48
#define ITERS  60

static int A[N][N];
static int B[N][N];
static int C[N][N];

void _start(void)
{
    unsigned int seed = 0x12345678u;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            seed = seed * 1664525u + 1013904223u;
            A[i][j] = (int)((seed >> 16) & 0x7FF);
            seed = seed * 1664525u + 1013904223u;
            B[i][j] = (int)((seed >> 16) & 0x7FF);
        }
    }

    int trace = 0;
    for (int it = 0; it < ITERS; it++) {
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                int sum = 0;
                for (int k = 0; k < N; k++)
                    sum += A[i][k] * B[k][j];
                C[i][j] = sum;
            }
        }
        /* Mix C back into A so the optimizer can't lift the loop. */
        for (int i = 0; i < N; i++)
            A[i][i] ^= C[i][i];
        trace ^= C[it & (N - 1)][(it * 7) & (N - 1)];
    }

    printf("trace=%d\n", trace);
    exit(0);
}
