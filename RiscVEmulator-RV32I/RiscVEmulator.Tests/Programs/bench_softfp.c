/* bench_softfp.c — Double-precision matrix multiply via the soft-float runtime.
 *
 * F/D opcodes trap as illegal on this CPU, so clang lowers every double op to
 * a __adddf3 / __muldf3 / __floatsidf libcall in Runtime/softfloat.c. That
 * makes this bench a function-call / libcall stress test: lots of JAL/JALR,
 * register spilling, function entry/exit churn — exactly the patterns where
 * the JIT's dispatcher overhead shows up.
 */

#include "libc.h"

#define N      24
#define ITERS  40

static double A[N][N];
static double B[N][N];
static double C[N][N];

void _start(void)
{
    unsigned int seed = 0x9E3779B9u;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            seed = seed * 1664525u + 1013904223u;
            A[i][j] = (double)(int)((seed >> 8) & 0x7FFF) / 128.0;
            seed = seed * 1664525u + 1013904223u;
            B[i][j] = (double)(int)((seed >> 8) & 0x7FFF) / 128.0;
        }
    }

    double trace = 0.0;
    for (int it = 0; it < ITERS; it++) {
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                double s = 0.0;
                for (int k = 0; k < N; k++)
                    s += A[i][k] * B[k][j];
                C[i][j] = s;
            }
        }
        trace += C[it & (N - 1)][it & (N - 1)];
    }

    /* Don't print the double directly — printf %f drags in more softfp.
     * An integer fingerprint is enough to verify correctness. */
    int fp = (int)(trace * 0.0001);
    printf("fp=%d\n", fp);
    exit(0);
}
