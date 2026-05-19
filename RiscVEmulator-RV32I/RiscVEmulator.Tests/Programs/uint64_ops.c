/* uint64_ops.c
 * Tests 64-bit integer operations on a 32-bit RV32IM target.
 * The compiler decomposes each 64-bit op into pairs of 32-bit instructions.
 *
 * Output: one result per line as "label=HI:LO\n" where HI and LO are
 * uppercase hex 32-bit values (zero-padded to 8 digits).
 */
#include "libc.h"

typedef unsigned long long u64;

void _start(void)
{
    volatile u64 a, b, r;

    /* ── Addition across 32-bit boundary ───────────────────── */
    a = 0xFFFFFFFFULL; b = 1ULL;
    r = a + b;                          /* 0x00000001:00000000 */
    printf("add=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Subtraction across 32-bit boundary ────────────────── */
    a = 0x100000000ULL; b = 1ULL;
    r = a - b;                          /* 0x00000000:FFFFFFFF */
    printf("sub=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Multiplication producing >32-bit result ───────────── */
    a = 0x10000ULL; b = 0x10000ULL;
    r = a * b;                          /* 0x00000001:00000000 */
    printf("mul=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Large multiplication ──────────────────────────────── */
    a = 100000ULL; b = 100000ULL;
    r = a * b;                          /* 10000000000 = 0x00000002:540BE400 */
    printf("mul2=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── XOR ───────────────────────────────────────────────── */
    a = 0xAAAAAAAA55555555ULL;
    b = 0x55555555AAAAAAAAULL;
    r = a ^ b;                          /* 0xFFFFFFFF:FFFFFFFF */
    printf("xor=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── AND ───────────────────────────────────────────────── */
    a = 0xFF00FF00FF00FF00ULL;
    b = 0x0FF00FF00FF00FF0ULL;
    r = a & b;                          /* 0x0F000F00:0F000F00 */
    printf("and=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── OR ────────────────────────────────────────────────── */
    a = 0xFF00000000000000ULL;
    b = 0x000000000000FF00ULL;
    r = a | b;                          /* 0xFF000000:0000FF00 */
    printf("or=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Shift left by 32 ──────────────────────────────────── */
    a = 1ULL;
    r = a << 32;                        /* 0x00000001:00000000 */
    printf("shl=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Shift right by 32 ─────────────────────────────────── */
    a = 0x8000000000000000ULL;
    r = a >> 32;                        /* 0x00000000:80000000 */
    printf("shr=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Shift left by 1 (carry across boundary) ───────────── */
    a = 0x80000000ULL;
    r = a << 1;                         /* 0x00000001:00000000 */
    printf("shl1=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── NOT (complement) ──────────────────────────────────── */
    a = 0x0000000000000000ULL;
    r = ~a;                             /* 0xFFFFFFFF:FFFFFFFF */
    printf("not=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    /* ── Subtraction producing negative (unsigned) ─────────── */
    a = 0ULL; b = 1ULL;
    r = a - b;                          /* 0xFFFFFFFF:FFFFFFFF */
    printf("neg=%08X:%08X\n", (unsigned int)(r >> 32), (unsigned int)r);

    exit(0);
}
