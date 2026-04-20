/* runtime.c
 * Software implementations of multiply and divide for pure RV32I.
 *
 * When compiling with -march=rv32i (no M extension), clang emits calls
 * to these compiler-rt ABI functions for any C code that uses *, /, or %.
 * All routines use only add, sub, shift, and branch — no MUL/DIV/REM.
 *
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O3 -c
 */

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                s32;
typedef long long          s64;

/* ── 32-bit unsigned multiply: shift-and-add ─────────────────────── */
u32 __mulsi3(u32 a, u32 b)
{
    u32 result = 0;
    while (a) {
        if (a & 1u) result += b;
        a >>= 1;
        b <<= 1;
    }
    return result;
}

/* ── 32-bit unsigned divide: bit-by-bit long division ────────────── *
 * Returns 0xFFFFFFFF for divide-by-zero (matches RISC-V DIVU spec). */
u32 __udivsi3(u32 n, u32 d)
{
    if (!d) return 0xFFFFFFFFu;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1u << i); }
    }
    return q;
}

/* ── 32-bit unsigned modulo ──────────────────────────────────────── *
 * Returns dividend for mod-by-zero (matches RISC-V REMU spec).      */
u32 __umodsi3(u32 n, u32 d)
{
    if (!d) return n;
    u32 r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) r -= d;
    }
    return r;
}

/* ── 32-bit signed divide ────────────────────────────────────────── */
s32 __divsi3(s32 a, s32 b)
{
    u32 neg = 0;
    u32 ua, ub;
    if (a < 0) { neg ^= 1; ua = (u32)(-a); } else { ua = (u32)a; }
    if (b < 0) { neg ^= 1; ub = (u32)(-b); } else { ub = (u32)b; }
    u32 q = __udivsi3(ua, ub);
    return neg ? -(s32)q : (s32)q;
}

/* ── 32-bit signed modulo ────────────────────────────────────────── */
s32 __modsi3(s32 a, s32 b)
{
    u32 neg = 0;
    u32 ua, ub;
    if (a < 0) { neg = 1; ua = (u32)(-a); } else { ua = (u32)a; }
    if (b < 0) { ub = (u32)(-b); } else { ub = (u32)b; }
    u32 r = __umodsi3(ua, ub);
    return neg ? -(s32)r : (s32)r;
}

/* ── 64-bit unsigned multiply ────────────────────────────────────── *
 * Used by softfloat.c (mul64hi) and any C code with uint64_t *.
 * Decomposes into four 32×32→64 partial products using __mulsi3.     */
u64 __muldi3(u64 a, u64 b)
{
    u32 a_lo = (u32)a,  a_hi = (u32)(a >> 32);
    u32 b_lo = (u32)b,  b_hi = (u32)(b >> 32);

    /* 32×32→64 using __mulsi3 for the 32-bit multiplies.
     * a_lo * b_lo can overflow 32 bits, so we split further:
     *   lo*lo = (lo_lo*lo_lo) + (lo_hi*lo_lo + lo_lo*lo_hi)<<16 + ... */
    u32 a_ll = a_lo & 0xFFFFu, a_lh = a_lo >> 16;
    u32 b_ll = b_lo & 0xFFFFu, b_lh = b_lo >> 16;

    /* 16×16 products (each fits in 32 bits) */
    u32 p0 = __mulsi3(a_ll, b_ll);
    u32 p1 = __mulsi3(a_ll, b_lh);
    u32 p2 = __mulsi3(a_lh, b_ll);
    u32 p3 = __mulsi3(a_lh, b_lh);

    /* Assemble a_lo * b_lo as 64-bit */
    u32 mid = (p0 >> 16) + (u32)(p1 & 0xFFFFu) + (u32)(p2 & 0xFFFFu);
    u32 lo  = (p0 & 0xFFFFu) | (mid << 16);
    u32 hi  = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);

    /* Cross terms: a_lo*b_hi + a_hi*b_lo (only low 32 bits matter for hi word) */
    hi += __mulsi3(a_lo, b_hi) + __mulsi3(a_hi, b_lo);

    return ((u64)hi << 32) | lo;
}

/* ═══════════════════════════════════════════════════════════════════
 * 64-BIT INTEGER DIVISION (compiler-rt ABI)
 * Needed when C code uses uint64_t / uint64_t on a 32-bit target.
 * ═══════════════════════════════════════════════════════════════════ */

u64 __udivdi3(u64 a, u64 b)
{
    if (!b) return 0;
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= (1ULL << i); }
    }
    return q;
}

u64 __umoddi3(u64 a, u64 b)
{
    if (!b) return 0;
    u64 r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) r -= b;
    }
    return r;
}

s64 __divdi3(s64 a, s64 b)
{
    u32 neg = 0;
    u64 ua, ub;
    if (a < 0) { neg ^= 1; ua = (u64)(-a); } else { ua = (u64)a; }
    if (b < 0) { neg ^= 1; ub = (u64)(-b); } else { ub = (u64)b; }
    u64 q = __udivdi3(ua, ub);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
    u32 neg = 0;
    u64 ua, ub;
    if (a < 0) { neg = 1; ua = (u64)(-a); } else { ua = (u64)a; }
    if (b < 0) { ub = (u64)(-b); } else { ub = (u64)b; }
    u64 r = __umoddi3(ua, ub);
    return neg ? -(s64)r : (s64)r;
}

/* ═══════════════════════════════════════════════════════════════════
 * 64-BIT SHIFT HELPERS (compiler-rt ABI)
 * Clang may emit these at -O0 for 64-bit shifts on a 32-bit target.
 * At -O1+ they're usually inlined, but we provide them for safety.
 * ═══════════════════════════════════════════════════════════════════ */

u64 __ashldi3(u64 a, int b)
{
    if (b == 0) return a;
    if (b >= 64) return 0;
    u32 lo = (u32)a;
    u32 hi = (u32)(a >> 32);
    if (b >= 32) {
        return (u64)(lo << (b - 32)) << 32;
    }
    u32 new_hi = (hi << b) | (lo >> (32 - b));
    u32 new_lo = lo << b;
    return ((u64)new_hi << 32) | new_lo;
}

u64 __lshrdi3(u64 a, int b)
{
    if (b == 0) return a;
    if (b >= 64) return 0;
    u32 lo = (u32)a;
    u32 hi = (u32)(a >> 32);
    if (b >= 32) {
        return hi >> (b - 32);
    }
    u32 new_lo = (lo >> b) | (hi << (32 - b));
    u32 new_hi = hi >> b;
    return ((u64)new_hi << 32) | new_lo;
}

s64 __ashrdi3(s64 a, int b)
{
    if (b == 0) return a;
    if (b >= 64) return (a < 0) ? -1LL : 0LL;
    u32 lo = (u32)(u64)a;
    s32 hi = (s32)((u64)a >> 32);
    if (b >= 32) {
        return (s64)(hi >> (b - 32));
    }
    u32 new_lo = (lo >> b) | ((u32)hi << (32 - b));
    s32 new_hi = hi >> b;
    return ((u64)(u32)new_hi << 32) | new_lo;
}

/* ═══════════════════════════════════════════════════════════════════
 * C LIBRARY STUBS
 * Needed when array initializers or struct copies cause the compiler
 * to emit memset/memcpy calls.
 * ═══════════════════════════════════════════════════════════════════ */
void *memset(void *dest, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}

void *memcpy(void *dest, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}
