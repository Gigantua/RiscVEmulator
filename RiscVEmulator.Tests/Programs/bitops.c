/* bitops.c
 * Tests every shift and bitwise instruction in RV32I.
 *
 * Key targets:
 *   SLLI, SRLI, SRAI — immediate forms, including SRAI on a negative (sign preserved)
 *   SLL, SRL, SRA    — register-operand forms
 *   XOR / XORI, OR / ORI, AND / ANDI
 *   Shift by 0 (no change) and shift by 31 (extreme)
 *   Popcount (OR+shift loop)
 *   Bit rotation emulation (SLL | SRL)
 */

#include "libc.h"

static void check(const char *name, int got, int expected)
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
    printf("%s: 0x%08x", name, got);
    if (got != expected)
        printf(" FAIL (expected 0x%08x)", expected);
    else
        printf(" OK");
    printf("\n");
}

/* Popcount using OR + shift — exercises SRL loop */
static int popcount(unsigned int x)
{
    int count = 0;
    while (x) { count += (int)(x & 1u); x >>= 1; }
    return count;
}

/* Rotate left 32 bits — exercises SLL | SRL */
static unsigned int rotl32(unsigned int x, unsigned int n)
{
    n &= 31u;
    return (x << n) | (x >> (32u - n));
}

void _start(void)
{
    int neg = -8;   /* 0xFFFFFFF8 */

    /* ── Immediate shifts ───────────────────────────────────────── */
    check  ("slli 1<<4",   1 << 4,             16);
    check_u("srli >>3",    0x80000000u >> 3,    0x10000000u);
    check  ("srai neg>>1", neg >> 1,            -4);          /* sign bit preserved */
    check  ("srai neg>>31",neg >> 31,           -1);          /* all ones */
    check  ("slli by 0",   neg << 0,            neg);
    check_u("srli by 31",  0x80000000u >> 31,   1u);

    /* ── Register-operand shifts ────────────────────────────────── */
    unsigned int shamt3 = 3u, shamt31 = 31u;
    check_u("sll  reg",    1u << shamt3,        8u);
    check_u("srl  reg",    0x80u >> shamt3,     0x10u);
    check  ("sra  reg neg",(int)0x80000000u >> (int)shamt3, (int)0xF0000000u);
    check_u("srl  by 31",  0x80000000u >> shamt31, 1u);
    check  ("sra  by 31",  (int)0x80000000u >> (int)shamt31, -1);

    /* ── Bitwise ────────────────────────────────────────────────── */
    check_u("xor",   0xAAAAAAAAu ^ 0x55555555u, 0xFFFFFFFFu);
    check_u("xori",  0xFFFFFFFFu ^ 0xFFu,       0xFFFFFF00u);
    check_u("or",    0xF0F0F0F0u | 0x0F0F0F0Fu, 0xFFFFFFFFu);
    check_u("ori",   0u | 0xABu,                0xABu);
    check_u("and",   0xFFFFFF00u & 0x00FFFFFFu, 0x00FFFF00u);
    check_u("andi",  0xFFFFFFFFu & 0x0Fu,       0x0Fu);

    /* ── Compound ops ───────────────────────────────────────────── */
    check("popcount(0xF0F0F0F0)", popcount(0xF0F0F0F0u), 16);
    check("popcount(0)",          popcount(0u),           0);
    check_u("rotl32(1,4)",        rotl32(1u, 4u),         16u);
    check_u("rotl32(0x80000000,1)", rotl32(0x80000000u, 1u), 1u);

    exit(0);
}
