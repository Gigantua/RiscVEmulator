/* double_matmul.c
 * Tests 3x3 double matrix operations and a 2x2 matrix inverse.
 *
 * All entries are small integers (1..9) whose products are exact in
 * IEEE 754 double.  The 2x2 inverse uses values whose inverse is
 * exactly representable (half-integers).
 *
 * Output: each matrix printed row-major, one integer or half-integer
 * (multiplied by 2 and cast to int) per line.
 */
#include "libc.h"

typedef double mat3d[3][3];
typedef double mat2d[2][2];

/* 3x3 double matrix multiply */
static void mat3d_mul(const mat3d A, const mat3d B, mat3d C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0.0;
            for (int k = 0; k < 3; k++)
                s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
}

/* 3x3 double matrix add */
static void mat3d_add(const mat3d A, const mat3d B, mat3d C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            C[i][j] = A[i][j] + B[i][j];
}

/* 3x3 double scalar multiply */
static void mat3d_scale(double s, const mat3d A, mat3d C)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            C[i][j] = s * A[i][j];
}

/* 2x2 matrix inverse:
 *   inv([[a,b],[c,d]]) = (1/det) * [[d,-b],[-c,a]]
 *   where det = ad - bc */
static void mat2d_inv(const mat2d A, mat2d B)
{
    double det = A[0][0]*A[1][1] - A[0][1]*A[1][0];
    double inv_det = 1.0 / det;
    B[0][0] =  A[1][1] * inv_det;
    B[0][1] = -A[0][1] * inv_det;
    B[1][0] = -A[1][0] * inv_det;
    B[1][1] =  A[0][0] * inv_det;
}

/* Print 3x3 matrix: one integer per line */
static void print_mat3d(const char *label, const mat3d M)
{
    printf("%s\n", label);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            printf("%d\n", (int)M[i][j]);
}

/* Print 2x2 matrix: values multiplied by 2, cast to int (handles half-integers) */
static void print_mat2d_x2(const char *label, const mat2d M)
{
    printf("%s\n", label);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            printf("%d\n", (int)(M[i][j] * 2.0));
}

void _start(void)
{
    mat3d A = { {1.0, 2.0, 3.0},
                {4.0, 5.0, 6.0},
                {7.0, 8.0, 9.0} };

    mat3d B = { {9.0, 8.0, 7.0},
                {6.0, 5.0, 4.0},
                {3.0, 2.0, 1.0} };

    mat3d I3 = { {1.0, 0.0, 0.0},
                 {0.0, 1.0, 0.0},
                 {0.0, 0.0, 1.0} };

    mat3d C;

    /* ── C = A * B ──────────────────────────────────────────── */
    /* [[ 30, 24, 18], [ 84, 69, 54], [138,114, 90]] */
    mat3d_mul(A, B, C);
    print_mat3d("AB", C);

    /* ── C = A * I3  (should equal A) ──────────────────────── */
    mat3d_mul(A, I3, C);
    print_mat3d("AI", C);

    /* ── C = A + A ──────────────────────────────────────────── */
    mat3d_add(A, A, C);
    print_mat3d("AA", C);

    /* ── C = 3.0 * A ────────────────────────────────────────── */
    mat3d_scale(3.0, A, C);
    print_mat3d("3A", C);

    /* ── 2x2 matrix inverse ─────────────────────────────────── */
    /* A2 = [[4,2],[3,1]], det = 4*1 - 2*3 = -2
     * inv = (1/-2)*[[1,-2],[-3,4]] = [[-0.5, 1.0],[1.5,-2.0]]
     * Print as *2: [-1, 2, 3, -4] */
    mat2d A2 = { {4.0, 2.0}, {3.0, 1.0} };
    mat2d invA2;
    mat2d_inv(A2, invA2);
    print_mat2d_x2("INV2x2", invA2);

    exit(0);
}
