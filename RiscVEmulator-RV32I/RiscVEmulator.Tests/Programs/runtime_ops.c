/* runtime_ops.c
 * Comprehensive test of all compiler-rt ABI functions in runtime.c:
 *   32-bit: __mulsi3, __udivsi3, __umodsi3, __divsi3, __modsi3
 *   64-bit: __muldi3, __udivdi3, __umoddi3, __divdi3, __moddi3
 *   64-bit shifts: __ashldi3, __lshrdi3, __ashrdi3
 *   C stubs: memset, memcpy
 *
 * Uses volatile variables to prevent constant-folding.
 * Output: "label=HI:LO OK\n" or "label=HI:LO FAIL\n"
 */
#include "libc.h"

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                s32;
typedef long long          s64;

static void check32(const char *label, u32 got, u32 expected)
{
    printf("%s=%08X", label, got);
    if (got == expected) printf(" OK\n");
    else printf(" FAIL expected=%08X\n", expected);
}

static void check64(const char *label, u64 got, u64 expected)
{
    printf("%s=%08X:%08X", label, (u32)(got >> 32), (u32)got);
    if (got == expected) printf(" OK\n");
    else printf(" FAIL expected=%08X:%08X\n", (u32)(expected >> 32), (u32)expected);
}

/* Volatile to defeat constant folding */
static volatile u32 vu7 = 7, vu3 = 3, vu0 = 0, vumax = 0xFFFFFFFFu;
static volatile s32 vs7 = 7, vsn7 = -7, vs3 = 3, vsn1 = -1;
static volatile u64 va64 = 100000ULL, vb64 = 100000ULL;
static volatile u64 vbig = 0x123456789ABCDEF0ULL;
static volatile u64 vdiv_a = 1000000000000ULL, vdiv_b = 7ULL;
static volatile s64 vs64a = -1000000000000LL, vs64b = 7LL;

void _start(void)
{
    printf("runtime_ops\n");

    /* ── 32-bit multiply ──────────────────────────────── */
    check32("mul7x3", vu7 * vu3, 21);
    check32("mul0", vu7 * vu0, 0);
    check32("mulmax", vumax * vu3, 0xFFFFFFFDu);

    /* ── 32-bit unsigned div/mod ──────────────────────── */
    check32("udiv7/3", vu7 / vu3, 2);
    check32("umod7%3", vu7 % vu3, 1);
    check32("udiv/0", vu7 / vu0, 0xFFFFFFFFu);
    check32("umod/0", vu7 % vu0, 7);

    /* ── 32-bit signed div/mod ────────────────────────── */
    check32("sdiv7/3", (u32)(vs7 / vs3), 2);
    check32("sdivn7/3", (u32)(vsn7 / vs3), (u32)(-2));
    check32("smod7%3", (u32)(vs7 % vs3), 1);
    check32("smodn7%3", (u32)(vsn7 % vs3), (u32)(-1));
    check32("sdiv7/-1", (u32)(vs7 / vsn1), (u32)(-7));

    /* ── 64-bit multiply ──────────────────────────────── */
    u64 m64 = va64 * vb64;  /* 10,000,000,000 = 0x00000002:540BE400 */
    check64("mul64", m64, 0x00000002540BE400ULL);

    /* ── 64-bit unsigned div/mod ──────────────────────── */
    u64 q = vdiv_a / vdiv_b;   /* 1000000000000 / 7 = 142857142857 */
    u64 r = vdiv_a % vdiv_b;   /* 1000000000000 % 7 = 1 */
    check64("udiv64", q, 142857142857ULL);
    check64("umod64", r, 1ULL);

    /* ── 64-bit signed div/mod ────────────────────────── */
    s64 sq = vs64a / vs64b;   /* -1000000000000 / 7 = -142857142857 */
    s64 sr = vs64a % vs64b;   /* -1000000000000 % 7 = -1 */
    check64("sdiv64", (u64)sq, (u64)(-142857142857LL));
    check64("smod64", (u64)sr, (u64)(-1LL));

    /* ── 64-bit left shift ────────────────────────────── */
    volatile u64 one = 1ULL;
    check64("shl0", one << 0, 1ULL);
    check64("shl1", one << 1, 2ULL);
    check64("shl16", one << 16, 0x10000ULL);
    check64("shl31", one << 31, 0x80000000ULL);
    check64("shl32", one << 32, 0x100000000ULL);
    check64("shl48", one << 48, 0x1000000000000ULL);
    check64("shl63", one << 63, 0x8000000000000000ULL);

    /* ── 64-bit logical right shift ───────────────────── */
    volatile u64 high = 0x8000000000000000ULL;
    check64("lshr0", high >> 0, 0x8000000000000000ULL);
    check64("lshr1", high >> 1, 0x4000000000000000ULL);
    check64("lshr32", high >> 32, 0x80000000ULL);
    check64("lshr63", high >> 63, 1ULL);

    /* ── 64-bit arithmetic right shift ────────────────── */
    volatile s64 neg = -2LL;  /* 0xFFFFFFFFFFFFFFFE */
    check64("ashr1", (u64)(neg >> 1), (u64)(-1LL));
    volatile s64 neg_big = (s64)0x8000000000000000ULL;
    check64("ashr32", (u64)(neg_big >> 32), (u64)(-2147483648LL));
    check64("ashr63", (u64)(neg_big >> 63), (u64)(-1LL));

    /* ── memset ───────────────────────────────────────── */
    volatile char buf[8];
    for (int i = 0; i < 8; i++) ((char *)buf)[i] = 0;
    __builtin_memset((void *)buf, 0xAB, 8);
    u32 ok = 1;
    for (int i = 0; i < 8; i++) if (buf[i] != (char)0xAB) ok = 0;
    printf("memset=%s\n", ok ? "OK" : "FAIL");

    /* ── memcpy ───────────────────────────────────────── */
    volatile char src[4] = {0x12, 0x34, 0x56, 0x78};
    volatile char dst[4] = {0, 0, 0, 0};
    __builtin_memcpy((void *)dst, (const void *)src, 4);
    ok = (dst[0] == 0x12 && dst[1] == 0x34 && dst[2] == 0x56 && dst[3] == 0x78);
    printf("memcpy=%s\n", ok ? "OK" : "FAIL");

    printf("runtime_done\n");

    exit(0);
}
