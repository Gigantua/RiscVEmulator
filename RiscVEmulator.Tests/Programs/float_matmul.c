/* float_matmul.c
 * Tests 3x3 float matrix operations: multiply, add, scalar scale.
 * Also tests identity matrix multiplication and a simple dot product.
 *
 * All matrix entries are small integers (1..9) whose products fit exactly
 * in IEEE 754 float32 (max result 138, well below 2^24).
 *
 * Output: each matrix result printed row-major, one integer per line.
 */
#include "libc.h"

typedef float mat3[3][3];

/* 3x3 matrix multiply: C = A * B */
static void mat3_mul(const mat3 A, const mat3 B, mat3 C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++)
                s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
}

/* 3x3 matrix add: C = A + B */
static void mat3_add(const mat3 A, const mat3 B, mat3 C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            C[i][j] = A[i][j] + B[i][j];
}

/* 3x3 scalar multiply: B = s * A */
static void mat3_scale(float s, const mat3 A, mat3 C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            C[i][j] = s * A[i][j];
}

/* Print all 9 elements of a 3x3 matrix, one per line */
static void print_mat3(const char *label, const mat3 M)
{
    printf("%s\n", label);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            printf("%d\n", (int)M[i][j]);
}

void _start(void)
{
    /* A = [[1,2,3],[4,5,6],[7,8,9]] */
    mat3 A = { {1.0f, 2.0f, 3.0f},
               {4.0f, 5.0f, 6.0f},
               {7.0f, 8.0f, 9.0f} };

    /* B = [[9,8,7],[6,5,4],[3,2,1]] */
    mat3 B = { {9.0f, 8.0f, 7.0f},
               {6.0f, 5.0f, 4.0f},
               {3.0f, 2.0f, 1.0f} };

    /* Identity matrix */
    mat3 I = { {1.0f, 0.0f, 0.0f},
               {0.0f, 1.0f, 0.0f},
               {0.0f, 0.0f, 1.0f} };

    mat3 C;

    /* ── C = A * B ─────────────────────────────────────────── */
    /* Expected:
     *   [[ 30, 24, 18],
     *    [ 84, 69, 54],
     *    [138,114, 90]] */
    mat3_mul(A, B, C);
    print_mat3("AB", C);

    /* ── C = A * I  (should equal A) ───────────────────────── */
    mat3_mul(A, I, C);
    print_mat3("AI", C);

    /* ── C = A + A  (should equal 2*A) ─────────────────────── */
    mat3_add(A, A, C);
    print_mat3("AA", C);

    /* ── C = 3.0 * A ────────────────────────────────────────── */
    mat3_scale(3.0f, A, C);
    print_mat3("3A", C);

    exit(0);
}
