/* softfloat.c
 * Pure-integer IEEE 754 single- and double-precision soft-float library.
 *
 * Implements every compiler-rt ABI routine emitted by clang when compiling
 * float/double code for a target with no hardware FPU (e.g. -march=rv32i
 * -mabi=ilp32 -nostdlib).
 *
 * All arithmetic uses only integer operations so this file never calls itself
 * recursively.  Rounding mode: round toward zero (truncation) — sufficient for
 * test values that are exactly representable in IEEE 754.
 *
 * Compile alongside user code; do NOT link any other float library.
 */

/* ─── Portable integer types (no stdint.h) ─────────────────────── */
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef int                 s32;
typedef long long           s64;

/* ─── Union type-punning (C99) ──────────────────────────────────── */
typedef union { float  f; u32 u; } f32u;
typedef union { double d; u64 u; } f64u;

/* ─── f32 constants ─────────────────────────────────────────────── */
#define F32_SIGN_MASK  0x80000000u
#define F32_EXP_MASK   0x7F800000u
#define F32_FRAC_MASK  0x007FFFFFu
#define F32_IMPL_BIT   0x00800000u   /* implicit leading 1 for normals */
#define F32_EXP_BIAS   127
#define F32_EXP_MAX    255
#define F32_NAN_BITS   0x7FC00000u
#define F32_INF_BITS   0x7F800000u

#define F32_SIGN(x)    ((x) >> 31)
#define F32_EXP(x)     (((x) >> 23) & 0xFFu)
#define F32_FRAC(x)    ((x) & 0x7FFFFFu)
#define F32_PACK(s,e,f) (((u32)(s)<<31) | ((u32)(e)<<23) | ((u32)(f) & 0x7FFFFFu))

/* ─── f64 constants ─────────────────────────────────────────────── */
#define F64_SIGN_MASK  0x8000000000000000ULL
#define F64_EXP_MASK   0x7FF0000000000000ULL
#define F64_FRAC_MASK  0x000FFFFFFFFFFFFFull
#define F64_IMPL_BIT   0x0010000000000000ULL
#define F64_EXP_BIAS   1023
#define F64_EXP_MAX    2047
#define F64_NAN_BITS   0x7FF8000000000000ULL
#define F64_INF_BITS   0x7FF0000000000000ULL

#define F64_SIGN(x)    ((u32)((x) >> 63))
#define F64_EXP(x)     ((u32)(((x) >> 52) & 0x7FFu))
#define F64_FRAC(x)    ((x) & 0x000FFFFFFFFFFFFFull)
#define F64_PACK(s,e,f) ((u64)(s)<<63 | (u64)(e)<<52 | ((u64)(f) & 0x000FFFFFFFFFFFFFull))

/* ─── Helper: count leading zeros ───────────────────────────────── */
static u32 clz32(u32 x)
{
    if (!x) return 32u;
    u32 n = 0;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n +=  8; x <<=  8; }
    if (!(x & 0xF0000000u)) { n +=  4; x <<=  4; }
    if (!(x & 0xC0000000u)) { n +=  2; x <<=  2; }
    if (!(x & 0x80000000u)) { n +=  1; }
    return n;
}

static u32 clz64(u64 x)
{
    u32 hi = (u32)(x >> 32);
    if (hi) return clz32(hi);
    u32 lo = (u32)x;
    return lo ? 32u + clz32(lo) : 64u;
}

/* ─── Helper: upper 64 bits of 64-bit × 64-bit (128-bit result) ── */
static u64 mul64hi(u64 a, u64 b)
{
    u32 a_lo = (u32)a,  a_hi = (u32)(a >> 32);
    u32 b_lo = (u32)b,  b_hi = (u32)(b >> 32);
    u64 ll = (u64)a_lo * b_lo;
    u64 lh = (u64)a_lo * b_hi;
    u64 hl = (u64)a_hi * b_lo;
    u64 hh = (u64)a_hi * b_hi;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* ─── f32 pack helper ───────────────────────────────────────────── */
/* Packs sign/exp/frac into a float, clamping to ±inf on overflow.
 * The implicit leading 1 bit (bit 23) in frac is masked away automatically
 * by F32_PACK's & 0x7FFFFF. */
static float pack_f32(u32 sign, s32 exp, u32 frac)
{
    f32u r;
    if (exp >= F32_EXP_MAX) { r.u = F32_PACK(sign, F32_EXP_MAX, 0u); return r.f; }
    if (exp <= 0)            { r.u = F32_PACK(sign, 0u, 0u);          return r.f; }
    r.u = F32_PACK(sign, (u32)exp, frac);
    return r.f;
}

/* ─── f64 pack helper ───────────────────────────────────────────── */
static double pack_f64(u32 sign, s32 exp, u64 frac)
{
    f64u r;
    if (exp >= F64_EXP_MAX) { r.u = F64_PACK(sign, (u64)F64_EXP_MAX, 0ULL); return r.d; }
    if (exp <= 0)            { r.u = F64_PACK(sign, 0ULL, 0ULL);              return r.d; }
    r.u = F64_PACK(sign, (u64)exp, frac);
    return r.d;
}

/* ═══════════════════════════════════════════════════════════════════
 * FLOAT (f32) ARITHMETIC
 * ═══════════════════════════════════════════════════════════════════ */

/* ── __addsf3 : float + float ─────────────────────────────────── */
float __addsf3(float fa, float fb)
{
    f32u ua = {.f = fa}, ub = {.f = fb};
    u32 a = ua.u, b = ub.u;
    u32 as = F32_SIGN(a), bs = F32_SIGN(b);
    s32 ae = (s32)F32_EXP(a), be = (s32)F32_EXP(b);
    u32 af = F32_FRAC(a), bf = F32_FRAC(b);

    /* NaN propagation */
    if (ae == F32_EXP_MAX) { f32u r; r.u = af ? (a | 0x400000u) : a; return r.f; }
    if (be == F32_EXP_MAX) { f32u r; r.u = bf ? (b | 0x400000u) : b; return r.f; }

    /* zero shortcut */
    if (!ae && !af) { f32u r; r.u = b; return r.f; }
    if (!be && !bf) { f32u r; r.u = a; return r.f; }

    /* implicit leading 1 for normals */
    if (ae) af |= F32_IMPL_BIT;
    if (be) bf |= F32_IMPL_BIT;

    /* make ae >= be */
    if (ae < be) {
        s32 te = ae; ae = be; be = te;
        u32 tf = af; af = bf; bf = tf;
        u32 ts = as; as = bs; bs = ts;
    }

    /* align smaller operand */
    s32 shift = ae - be;
    bf = (shift >= 25) ? 0u : (bf >> shift);

    u32 rs; u32 rf; s32 re = ae;

    if (as == bs) {
        rf = af + bf;
        rs = as;
        if (rf & 0x1000000u) { rf >>= 1; re++; }   /* mantissa carry */
    } else {
        if (af >= bf) { rf = af - bf; rs = as; }
        else          { rf = bf - af; rs = bs; }
        if (!rf) { f32u r; r.u = 0u; return r.f; }  /* exact cancel */
        /* normalize: shift left until implicit 1 is at bit 23 */
        while (!(rf & F32_IMPL_BIT) && re > 1) { rf <<= 1; re--; }
    }

    return pack_f32(rs, re, rf);
}

/* ── __subsf3 : float - float ─────────────────────────────────── */
float __subsf3(float a, float b)
{
    f32u ub = {.f = b};
    ub.u ^= F32_SIGN_MASK;
    return __addsf3(a, ub.f);
}

/* ── __mulsf3 : float * float ─────────────────────────────────── */
float __mulsf3(float fa, float fb)
{
    f32u ua = {.f = fa}, ub = {.f = fb};
    u32 a = ua.u, b = ub.u;
    u32 rs = F32_SIGN(a) ^ F32_SIGN(b);
    s32 ae = (s32)F32_EXP(a), be = (s32)F32_EXP(b);
    u32 af = F32_FRAC(a), bf = F32_FRAC(b);

    if (ae == F32_EXP_MAX || be == F32_EXP_MAX) {
        int a_nan  = (ae == F32_EXP_MAX && af), b_nan  = (be == F32_EXP_MAX && bf);
        int a_zero = (!ae && !af),               b_zero = (!be && !bf);
        if (a_nan || b_nan || a_zero || b_zero) { f32u r; r.u = F32_NAN_BITS; return r.f; }
        return pack_f32(rs, F32_EXP_MAX, 0u);   /* inf * nonzero = inf */
    }
    if ((!ae && !af) || (!be && !bf)) return pack_f32(rs, 0, 0u); /* 0 * x */

    af |= F32_IMPL_BIT; bf |= F32_IMPL_BIT;

    /* 24-bit × 24-bit = 48-bit product */
    u64 prod = (u64)af * (u64)bf;
    s32 re   = ae + be - F32_EXP_BIAS;
    u32 rf;

    if (prod & (1ULL << 47)) { rf = (u32)(prod >> 24); re++; }
    else                     { rf = (u32)(prod >> 23); }

    return pack_f32(rs, re, rf);
}

/* ── __divsf3 : float / float ─────────────────────────────────── */
float __divsf3(float fa, float fb)
{
    f32u ua = {.f = fa}, ub = {.f = fb};
    u32 a = ua.u, b = ub.u;
    u32 rs = F32_SIGN(a) ^ F32_SIGN(b);
    s32 ae = (s32)F32_EXP(a), be = (s32)F32_EXP(b);
    u32 af = F32_FRAC(a), bf = F32_FRAC(b);

    if ((ae == F32_EXP_MAX && af) || (be == F32_EXP_MAX && bf)) { f32u r; r.u = F32_NAN_BITS; return r.f; }
    if (ae == F32_EXP_MAX) return pack_f32(rs, F32_EXP_MAX, 0u); /* inf / finite = inf */
    if (be == F32_EXP_MAX) return pack_f32(rs, 0, 0u);           /* finite / inf = 0 */
    if (!ae && !af) {
        if (!be && !bf) { f32u r; r.u = F32_NAN_BITS; return r.f; } /* 0/0 = NaN */
        return pack_f32(rs, 0, 0u);               /* 0 / nonzero = 0 */
    }
    if (!be && !bf) return pack_f32(rs, F32_EXP_MAX, 0u);        /* x / 0 = inf */

    af |= F32_IMPL_BIT; bf |= F32_IMPL_BIT;

    /* Compute q = floor(af * 2^24 / bf) using pure 32-bit long division.
     * af, bf in [2^23, 2^24).  q in [2^23, 2^25).
     * First bit: is af / bf >= 1?  (i.e. af >= bf) */
    u32 q_hi = (af >= bf) ? 1u : 0u;
    u32 q_rem = af - q_hi * bf;          /* remainder, < bf < 2^24 */
    u32 q_lo = 0;
    for (int _i = 0; _i < 24; _i++) {
        q_lo <<= 1;
        q_rem <<= 1;                     /* q_rem < 2^25, fits in u32 */
        if (q_rem >= bf) { q_rem -= bf; q_lo |= 1u; }
    }
    u32 q   = (q_hi << 24) | q_lo;
    s32 re;

    if (q >= (1u << 24)) { q >>= 1; re = ae - be + F32_EXP_BIAS; }
    else                 {          re = ae - be + F32_EXP_BIAS - 1; }

    return pack_f32(rs, re, q);
}

/* ═══════════════════════════════════════════════════════════════════
 * FLOAT (f32) COMPARISONS
 * Returns:  -1 if a < b,  0 if a == b,  1 if a > b,  2 if unordered
 *
 * On ilp32 ABI (no F extension), float is passed in integer registers.
 * We declare parameters as u32 to prevent clang from recognising these
 * as compiler-rt builtins and replacing their bodies.
 * ═══════════════════════════════════════════════════════════════════ */
static s32 f32_cmp_u(u32 a, u32 b)
{
    /* NaN → unordered */
    if (F32_EXP(a) == F32_EXP_MAX && F32_FRAC(a)) return 2;
    if (F32_EXP(b) == F32_EXP_MAX && F32_FRAC(b)) return 2;

    /* +0 == -0 */
    if ((a & 0x7FFFFFFFu) == 0 && (b & 0x7FFFFFFFu) == 0) return 0;

    /* same sign: compare magnitude */
    u32 sa = F32_SIGN(a), sb = F32_SIGN(b);
    if (sa != sb) return sa ? -1 : 1; /* negative < positive */

    u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    if (ma == mb) return 0;
    s32 cmp = (ma < mb) ? -1 : 1;
    return sa ? -cmp : cmp;  /* flip for negatives */
}

int __eqsf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c == 0) ? 0 : 1; }
int __nesf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c != 0) ? 1 : 0; }
int __ltsf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c <  0) ? -1 : (c == 0 ? 0 : 1); }
int __lesf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c <= 0) ? -1 : 1; }
int __gtsf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c >  0) ?  1 : (c == 0 ? 0 : -1); }
int __gesf2(u32 a, u32 b) { s32 c = f32_cmp_u(a,b); return (c >= 0) ?  1 : -1; }
int __unordsf2(u32 a, u32 b) { return (f32_cmp_u(a,b) == 2) ? 1 : 0; }

/* ═══════════════════════════════════════════════════════════════════
 * FLOAT (f32) CONVERSIONS
 * ═══════════════════════════════════════════════════════════════════ */

float __floatsisf(s32 x)
{
    if (!x) { f32u r; r.u = 0u; return r.f; }
    u32 sign = (x < 0) ? 1u : 0u;
    u32 abs  = sign ? (u32)(-x) : (u32)x;
    u32 lead = 31u - clz32(abs);         /* position of leading 1 */
    s32 exp  = (s32)lead + F32_EXP_BIAS;
    u32 frac;
    if (lead >= 23u) frac = abs >> (lead - 23u);
    else             frac = abs << (23u - lead);
    return pack_f32(sign, exp, frac);
}

float __floatunsisf(u32 x)
{
    if (!x) { f32u r; r.u = 0u; return r.f; }
    u32 lead = 31u - clz32(x);
    s32 exp  = (s32)lead + F32_EXP_BIAS;
    u32 frac;
    if (lead >= 23u) frac = x >> (lead - 23u);
    else             frac = x << (23u - lead);
    return pack_f32(0u, exp, frac);
}

s32 __fixsfsi(float fa)
{
    f32u ua = {.f = fa};
    u32 a = ua.u;
    u32 sign = F32_SIGN(a);
    s32 exp  = (s32)F32_EXP(a) - F32_EXP_BIAS;
    u32 frac = F32_FRAC(a) | F32_IMPL_BIT;
    if (exp < 0)  return 0;
    if (exp > 30) return sign ? (s32)0x80000000 : (s32)0x7FFFFFFF;
    u32 result = (exp >= 23) ? (frac << (exp - 23)) : (frac >> (23 - exp));
    return sign ? -(s32)result : (s32)result;
}

u32 __fixunssfsi(float fa)
{
    f32u ua = {.f = fa};
    u32 a = ua.u;
    if (F32_SIGN(a)) return 0u;
    s32 exp  = (s32)F32_EXP(a) - F32_EXP_BIAS;
    u32 frac = F32_FRAC(a) | F32_IMPL_BIT;
    if (exp < 0)  return 0u;
    if (exp > 31) return 0xFFFFFFFFu;
    return (exp >= 23) ? (frac << (exp - 23)) : (frac >> (23 - exp));
}

/* ═══════════════════════════════════════════════════════════════════
 * DOUBLE (f64) ARITHMETIC
 * ═══════════════════════════════════════════════════════════════════ */

/* ── __adddf3 : double + double ───────────────────────────────── */
double __adddf3(double fa, double fb)
{
    f64u ua = {.d = fa}, ub = {.d = fb};
    u64 a = ua.u, b = ub.u;
    u32 as = F64_SIGN(a), bs = F64_SIGN(b);
    s32 ae = (s32)F64_EXP(a), be = (s32)F64_EXP(b);
    u64 af = F64_FRAC(a), bf = F64_FRAC(b);

    if (ae == F64_EXP_MAX) { f64u r; r.u = af ? (a | 0x0008000000000000ULL) : a; return r.d; }
    if (be == F64_EXP_MAX) { f64u r; r.u = bf ? (b | 0x0008000000000000ULL) : b; return r.d; }

    if (!ae && !af) { f64u r; r.u = b; return r.d; }
    if (!be && !bf) { f64u r; r.u = a; return r.d; }

    if (ae) af |= F64_IMPL_BIT;
    if (be) bf |= F64_IMPL_BIT;

    if (ae < be) {
        s32 te = ae; ae = be; be = te;
        u64 tf = af; af = bf; bf = tf;
        u32 ts = as; as = bs; bs = ts;
    }

    s32 shift = ae - be;
    bf = (shift >= 54) ? 0ULL : (bf >> shift);

    u32 rs; u64 rf; s32 re = ae;

    if (as == bs) {
        rf = af + bf;
        rs = as;
        if (rf & (1ULL << 53)) { rf >>= 1; re++; }
    } else {
        if (af >= bf) { rf = af - bf; rs = as; }
        else          { rf = bf - af; rs = bs; }
        if (!rf) { f64u r; r.u = 0ULL; return r.d; }
        while (!(rf & F64_IMPL_BIT) && re > 1) { rf <<= 1; re--; }
    }

    return pack_f64(rs, re, rf);
}

/* ── __subdf3 : double - double ───────────────────────────────── */
double __subdf3(double a, double b)
{
    f64u ub = {.d = b};
    ub.u ^= F64_SIGN_MASK;
    return __adddf3(a, ub.d);
}

/* ── __muldf3 : double * double ───────────────────────────────── */
double __muldf3(double fa, double fb)
{
    f64u ua = {.d = fa}, ub = {.d = fb};
    u64 a = ua.u, b = ub.u;
    u32 rs = F64_SIGN(a) ^ F64_SIGN(b);
    s32 ae = (s32)F64_EXP(a), be = (s32)F64_EXP(b);
    u64 af = F64_FRAC(a), bf = F64_FRAC(b);

    if (ae == F64_EXP_MAX || be == F64_EXP_MAX) {
        int a_nan  = (ae == F64_EXP_MAX && af), b_nan  = (be == F64_EXP_MAX && bf);
        int a_zero = (!ae && !af),               b_zero = (!be && !bf);
        if (a_nan || b_nan || a_zero || b_zero) { f64u r; r.u = F64_NAN_BITS; return r.d; }
        return pack_f64(rs, F64_EXP_MAX, 0ULL);
    }
    if ((!ae && !af) || (!be && !bf)) return pack_f64(rs, 0, 0ULL);

    af |= F64_IMPL_BIT; bf |= F64_IMPL_BIT;

    /* Both af and bf are in [2^52, 2^53).
     * 53-bit × 53-bit product is in [2^104, 2^106).
     * prod_hi = upper 64 bits of 128-bit product → bits [127:64].
     * Since product < 2^106, prod_hi has leading 1 at bit 41 or bit 40. */
    u64 prod_hi = mul64hi(af, bf);
    u64 prod_lo = af * bf;
    s32 re = ae + be - F64_EXP_BIAS;
    u64 rf;

    if (prod_hi & (1ULL << 41)) {
        /* Leading 1 at product bit 105 → mantissa in [2, 4), renormalize +1.
         * Extract product bits [105:53] → rf[52:0]:
         *   bits [105:64] = prod_hi[41:0]  (42 bits → rf[52:11])
         *   bits [63:53]  = prod_lo[63:53] (11 bits → rf[10:0])   */
        rf = (prod_hi << 11) | (prod_lo >> 53);
        re += 1;
    } else {
        /* Leading 1 at product bit 104 → mantissa in [1, 2), exponent unchanged.
         * Extract product bits [104:52] → rf[52:0]:
         *   bits [104:64] = prod_hi[40:0]  (41 bits → rf[52:12])
         *   bits [63:52]  = prod_lo[63:52] (12 bits → rf[11:0])   */
        rf = (prod_hi << 12) | (prod_lo >> 52);
    }

    return pack_f64(rs, re, rf);
}

/* ── __divdf3 : double / double ───────────────────────────────── */
double __divdf3(double fa, double fb)
{
    f64u ua = {.d = fa}, ub = {.d = fb};
    u64 a = ua.u, b = ub.u;
    u32 rs = F64_SIGN(a) ^ F64_SIGN(b);
    s32 ae = (s32)F64_EXP(a), be = (s32)F64_EXP(b);
    u64 af = F64_FRAC(a), bf = F64_FRAC(b);

    if ((ae == F64_EXP_MAX && af) || (be == F64_EXP_MAX && bf)) { f64u r; r.u = F64_NAN_BITS; return r.d; }
    if (ae == F64_EXP_MAX) return pack_f64(rs, F64_EXP_MAX, 0ULL);
    if (be == F64_EXP_MAX) return pack_f64(rs, 0, 0ULL);
    if (!ae && !af) {
        if (!be && !bf) { f64u r; r.u = F64_NAN_BITS; return r.d; }
        return pack_f64(rs, 0, 0ULL);
    }
    if (!be && !bf) return pack_f64(rs, F64_EXP_MAX, 0ULL);

    af |= F64_IMPL_BIT; bf |= F64_IMPL_BIT;

    /* Long division: compute q = floor(af * 2^53 / bf).
     * af, bf ∈ [2^52, 2^53).  Result q ∈ [2^52, 2^54).
     * First determine the top bit (bit 53): af >= bf means quotient >= 2^53. */
    u64 q;
    u64 rem;
    if (af >= bf) { q = 1ULL << 53; rem = af - bf; }
    else          { q = 0ULL;       rem = af; }
    /* 53 iterations for bits 52 down to 0. rem < bf invariant maintained. */
    for (int i = 52; i >= 0; i--) {
        rem <<= 1;
        if (rem >= bf) { rem -= bf; q |= (1ULL << i); }
    }

    /* q is in [2^52, 2^54) — normalize to have leading 1 at bit 52 */
    s32 re;
    if (q & (1ULL << 53)) { q >>= 1; re = ae - be + F64_EXP_BIAS; }
    else                  {          re = ae - be + F64_EXP_BIAS - 1; }

    return pack_f64(rs, re, q);
}

/* ═══════════════════════════════════════════════════════════════════
 * DOUBLE (f64) COMPARISONS
 *
 * On ilp32 ABI (no D extension), double is passed in a0:a1 (two
 * integer registers) — the same layout as u64.  We use u64 parameters
 * so clang cannot recognise these as compiler-rt builtins.
 * ═══════════════════════════════════════════════════════════════════ */
static s32 f64_cmp_u(u64 a, u64 b)
{
    if (F64_EXP(a) == F64_EXP_MAX && F64_FRAC(a)) return 2;
    if (F64_EXP(b) == F64_EXP_MAX && F64_FRAC(b)) return 2;

    if ((a & ~F64_SIGN_MASK) == 0 && (b & ~F64_SIGN_MASK) == 0) return 0; /* ±0 == ±0 */

    u32 sa = F64_SIGN(a), sb = F64_SIGN(b);
    if (sa != sb) return sa ? -1 : 1;

    u64 ma = a & ~F64_SIGN_MASK, mb = b & ~F64_SIGN_MASK;
    if (ma == mb) return 0;
    s32 cmp = (ma < mb) ? -1 : 1;
    return sa ? -cmp : cmp;
}

int __eqdf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c == 0) ? 0 : 1; }
int __nedf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c != 0) ? 1 : 0; }
int __ltdf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c <  0) ? -1 : (c == 0 ? 0 : 1); }
int __ledf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c <= 0) ? -1 : 1; }
int __gtdf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c >  0) ?  1 : (c == 0 ? 0 : -1); }
int __gedf2(u64 a, u64 b) { s32 c = f64_cmp_u(a,b); return (c >= 0) ?  1 : -1; }
int __unorddf2(u64 a, u64 b) { return (f64_cmp_u(a,b) == 2) ? 1 : 0; }

/* ═══════════════════════════════════════════════════════════════════
 * DOUBLE (f64) CONVERSIONS
 * ═══════════════════════════════════════════════════════════════════ */

double __floatsidf(s32 x)
{
    if (!x) { f64u r; r.u = 0ULL; return r.d; }
    u32 sign = (x < 0) ? 1u : 0u;
    u32 abs  = sign ? (u32)(-x) : (u32)x;
    u32 lead = 31u - clz32(abs);
    s32 exp  = (s32)lead + F64_EXP_BIAS;
    u64 frac;
    if (lead >= 52u) frac = (u64)abs >> (lead - 52u);
    else             frac = (u64)abs << (52u - lead);
    return pack_f64(sign, exp, frac);
}

double __floatunsidf(u32 x)
{
    if (!x) { f64u r; r.u = 0ULL; return r.d; }
    u32 lead = 31u - clz32(x);
    s32 exp  = (s32)lead + F64_EXP_BIAS;
    u64 frac;
    if (lead >= 52u) frac = (u64)x >> (lead - 52u);
    else             frac = (u64)x << (52u - lead);
    return pack_f64(0u, exp, frac);
}

s32 __fixdfsi(double fa)
{
    f64u ua = {.d = fa};
    u64 a = ua.u;
    u32 sign = F64_SIGN(a);
    s32 exp  = (s32)F64_EXP(a) - F64_EXP_BIAS;
    u64 frac = F64_FRAC(a) | F64_IMPL_BIT;
    if (exp < 0)  return 0;
    if (exp > 30) return sign ? (s32)0x80000000 : (s32)0x7FFFFFFF;
    u32 result = (exp >= 52) ? (u32)(frac << (exp - 52)) : (u32)(frac >> (52 - exp));
    return sign ? -(s32)result : (s32)result;
}

u32 __fixunsdfsi(double fa)
{
    f64u ua = {.d = fa};
    u64 a = ua.u;
    if (F64_SIGN(a)) return 0u;
    s32 exp  = (s32)F64_EXP(a) - F64_EXP_BIAS;
    u64 frac = F64_FRAC(a) | F64_IMPL_BIT;
    if (exp < 0)  return 0u;
    if (exp > 31) return 0xFFFFFFFFu;
    return (exp >= 52) ? (u32)(frac << (exp - 52)) : (u32)(frac >> (52 - exp));
}

/* ═══════════════════════════════════════════════════════════════════
 * CROSS-WIDTH CONVERSIONS
 * ═══════════════════════════════════════════════════════════════════ */

double __extendsfdf2(float fa)
{
    f32u ua = {.f = fa};
    u32 a = ua.u;
    u32 sign = F32_SIGN(a);
    s32 exp  = (s32)F32_EXP(a);
    u32 frac = F32_FRAC(a);

    if (exp == F32_EXP_MAX) { /* NaN or Inf */
        f64u r;
        r.u = F64_PACK(sign, F64_EXP_MAX, frac ? ((u64)frac << 29) : 0ULL);
        return r.d;
    }
    if (!exp && !frac) { f64u r; r.u = (u64)sign << 63; return r.d; } /* ±0 */

    s32 new_exp  = exp - F32_EXP_BIAS + F64_EXP_BIAS;
    u64 new_frac = (u64)frac << 29;  /* 23-bit → 52-bit: shift left 29 */
    return pack_f64(sign, new_exp, new_frac);
}

float __truncdfsf2(double da)
{
    f64u ua = {.d = da};
    u64 a = ua.u;
    u32 sign = F64_SIGN(a);
    s32 exp  = (s32)F64_EXP(a);
    u64 frac = F64_FRAC(a);

    if (exp == F64_EXP_MAX) {
        f32u r; r.u = frac ? F32_NAN_BITS : F32_PACK(sign, F32_EXP_MAX, 0u);
        return r.f;
    }
    if (!exp && !frac) { f32u r; r.u = sign << 31; return r.f; }

    s32 new_exp  = exp - F64_EXP_BIAS + F32_EXP_BIAS;
    u32 new_frac = (u32)(frac >> 29);  /* 52-bit → 23-bit: shift right 29 */
    return pack_f32(sign, new_exp, new_frac);
}

/* 64-bit integer division and C library stubs are in runtime.c */
