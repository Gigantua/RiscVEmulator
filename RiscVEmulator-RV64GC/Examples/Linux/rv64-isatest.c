/*
 * rv64-isatest.c — bare-metal RV64GC ISA self-test for the rv64gc_core.
 *
 * Compiled `-march=rv64imafdc -mabi=lp64d` (the full ISA the core claims),
 * loaded flat at 0x80000000 and run in M-mode. Exercises the FP/D, M, A,
 * W-variant and shift paths and reports PASS/FAIL per case over the UART,
 * then writes the failure count to the host-exit device.
 *
 * Runs in well under a second — a fast bug-finder, unlike a Linux boot.
 */
#include <stdint.h>

#define UART 0x10000000UL
#define EXIT 0x40000000UL

static void wc(char c)            { *(volatile uint8_t *)UART = (uint8_t)c; }
static void ws(const char *s)     { while (*s) wc(*s++); }
static void whex(uint64_t v)
{
    ws("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        wc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
    }
}

static int g_pass = 0, g_fail = 0;
static void check(const char *name, uint64_t got, uint64_t exp)
{
    ws(name);
    ws(": ");
    if (got == exp) { ws("PASS\n"); g_pass++; }
    else { ws("FAIL got="); whex(got); ws(" exp="); whex(exp); wc('\n'); g_fail++; }
}

static uint64_t d2u(double d)  { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
static double   u2d(uint64_t u){ double d;   __builtin_memcpy(&d, &u, 8); return d; }
static uint32_t f2u(float f)   { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }

void test_main(void)
{
    ws("=== RV64GC ISA SELF-TEST ===\n");

    /* ── double arithmetic ── */
    volatile double a = 3.0, b = 2.0;
    check("fadd.d", d2u(a + b), d2u(5.0));
    check("fsub.d", d2u(a - b), d2u(1.0));
    check("fmul.d", d2u(a * b), d2u(6.0));
    check("fdiv.d", d2u(a / b), d2u(1.5));

    /* ── FMV.X.D / FMV.D.X — the 64-bit move must not truncate ── */
    volatile uint64_t pat = 0x123456789ABCDEF0ULL;
    double dpat = u2d(pat);            /* fmv.d.x */
    check("fmv.d.x/x.d", d2u(dpat), pat);
    /* a computed double forced through a GPR — exercises fmv.x.d directly */
    volatile double big = 12345.0;
    check("fmv.x.d", d2u(big * 2.0), d2u(24690.0));

    /* ── FCVT double<->int both directions ── */
    volatile int64_t  li = 1234567;
    check("fcvt.d.l",  d2u((double)li), d2u(1234567.0));
    volatile double   dv = 9876.0;
    check("fcvt.l.d",  (uint64_t)(int64_t)dv, 9876);
    volatile uint64_t lu = 0xFFFFFFFFULL;
    check("fcvt.d.lu", d2u((double)lu), d2u(4294967295.0));

    /* ── single precision + NaN-boxing ── */
    volatile float fa = 1.5f, fb = 0.25f;
    check("fadd.s",    f2u(fa + fb), f2u(1.75f));
    volatile double sd = 2.5;
    check("fcvt.s.d",  f2u((float)sd), f2u(2.5f));

    /* ── FMADD.d ── */
    volatile double m1 = 2.0, m2 = 3.0, m3 = 4.0;
    check("fmadd.d", d2u(m1 * m2 + m3), d2u(10.0));

    /* ── M extension, 64-bit ── */
    volatile uint64_t x = 0x100000000ULL, y = 3;
    check("mul64", x * y, 0x300000000ULL);
    volatile int64_t s1 = -100, s2 = 7;
    check("div64", (uint64_t)(s1 / s2), (uint64_t)(int64_t)-14);
    check("rem64", (uint64_t)(s1 % s2), (uint64_t)(int64_t)-2);

    /* ── W-variant (32-bit result, sign-extended) ── */
    volatile uint32_t w1 = 0x7FFFFFFF, w2 = 1;
    check("addw", (uint64_t)(int64_t)(int32_t)(w1 + w2), 0xFFFFFFFF80000000ULL);
    volatile uint32_t wm = 100000;
    check("mulw", (uint64_t)(int64_t)(int32_t)(wm * wm), 0x540BE400ULL);
    volatile uint32_t u1 = 1;
    check("sllw", (uint64_t)(int64_t)(int32_t)(u1 << 31), (uint64_t)(int64_t)(int32_t)0x80000000);

    /* ── 64-bit shifts (6-bit shamt) ── */
    volatile uint64_t sv = 1;
    check("sll64", sv << 40, 0x10000000000ULL);
    volatile int64_t  sr = -256;
    check("sra64", (uint64_t)(sr >> 4), (uint64_t)(int64_t)-16);

    /* ── A extension — AMOs ── */
    static volatile uint64_t amo = 10;
    uint64_t old = __atomic_fetch_add(&amo, 5, __ATOMIC_SEQ_CST);
    check("amoadd.d.old", old, 10);
    check("amoadd.d.new", amo, 15);
    static volatile uint32_t amo32 = 0x60;
    uint32_t o32 = __atomic_fetch_or(&amo32, 0xF, __ATOMIC_SEQ_CST);
    check("amoor.w", ((uint64_t)o32 << 32) | amo32, ((uint64_t)0x60 << 32) | 0x6F);

    ws("=== DONE pass=");
    whex((uint64_t)g_pass);
    ws(" fail=");
    whex((uint64_t)g_fail);
    ws(" ===\n");

    *(volatile uint32_t *)EXIT = (uint32_t)g_fail;   /* halt, exit code = #fails */
    for (;;) { }
}

__attribute__((section(".text.start"), naked, used))
void _start(void)
{
    __asm__ volatile("li sp, 0x82000000\n\t tail test_main\n");
}
