/* signed_ops.c — tests signed arithmetic, SLT/SLTU, and M-extension edge cases.
 * NOTE: all string literals use only plain ASCII.
 */

#include "libc.h"

static void check_i(const char *name, int got, int expected)
{
    printf("%s: %d", name, got);
    if (got != expected)
        printf(" FAIL (expected %d)", expected);
    else
        printf(" OK");
    printf("\n");
}

static void check_u(const char *name, unsigned int got, unsigned int expected)
{
    printf("%s: %u", name, got);
    if (got != expected)
        printf(" FAIL (expected %u)", expected);
    else
        printf(" OK");
    printf("\n");
}

/* volatile divisors defeat constant-folding so clang emits real div/rem instructions */
static volatile int vn1     = -1;
static volatile int v0      =  0;
static volatile int v3      =  3;
static volatile int v7      =  7;
static volatile int vINTMIN = (int)0x80000000;

void _start(void)
{
    /* ── Signed DIV ─────────────────────────────────────────────── */
    check_i("7 div 3",     7  / v3,   2);
    check_i("7 div -1",    7  / vn1, -7);
    check_i("-7 div 3",   -7  / v3,  -2);
    check_i("-7 div -1",  -7  / vn1,  7);

    /* ── Signed REM ─────────────────────────────────────────────── */
    check_i("7 rem 3",     7  % v3,   1);
    check_i("-7 rem 3",   -7  % v3,  -1);   /* C99: truncation toward zero */
    check_i("7 rem -1",    7  % vn1,  0);
    check_i("-7 rem -1",  -7  % vn1,  0);

    /* ── M-extension edge cases ─────────────────────────────────── */
    check_i("div/0 = -1",        (int)(v7   / (unsigned int)v0), -1);
    check_u("divu/0 = UMAX",     (unsigned int)v7 / (unsigned int)v0, 0xFFFFFFFFu);
    check_i("rem/0 = dividend",  v7   % (int)v0,           7);
    check_u("remu/0 = dividend", (unsigned int)v7 % (unsigned int)v0, 7u);
    check_i("INT_MIN/-1=INT_MIN",vINTMIN / vn1,             (int)0x80000000);

    /* ── MULH / MULHSU / MULHU (using C-level 64-bit to verify) ── */
    /* (-2^31)^2 = 2^62; upper 32 bits = 0x40000000 */
    check_i("mulh(INT_MIN,INT_MIN)",
        (int)(((long long)vINTMIN * vINTMIN) >> 32), 0x40000000);

    /* 0xFFFFFFFF * 0xFFFFFFFF upper 32 = 0xFFFFFFFE */
    volatile unsigned int umax = 0xFFFFFFFFu;
    check_u("mulhu(UMAX,UMAX)",
        (unsigned int)(((unsigned long long)umax * umax) >> 32), 0xFFFFFFFEu);

    /* signed(-1) * unsigned(2) upper 32 = 0xFFFFFFFF */
    volatile unsigned int u2 = 2u;
    check_u("mulhsu(-1,2)",
        (unsigned int)(((long long)vn1 * (long long)u2) >> 32), 0xFFFFFFFFu);

    /* ── SLT vs SLTU ─────────────────────────────────────────────── */
    int neg1 = vn1;
    int zero = v0;
    check_i("slt -1<0",   (neg1 < zero) ? 1 : 0,                    1);
    check_i("sltu -1<0u", ((unsigned int)neg1 < (unsigned int)zero) ? 1 : 0, 0);
    check_i("slt 0<-1",   (zero < neg1) ? 1 : 0,                    0);

    /* ── Signed branches with negatives ──────────────────────────── */
    check_i("blt -1<1",   (neg1 < 1)   ? 1 : 0, 1);
    check_i("bge 1>=-1",  (1 >= neg1)  ? 1 : 0, 1);

    /* ── SRAI on negative ─────────────────────────────────────────── */
    volatile int negval = -8;
    check_i("srai -8>>1=-4", negval >> 1, -4);
    check_i("srai -8>>31=-1",negval >> 31, -1);

    exit(0);
}
