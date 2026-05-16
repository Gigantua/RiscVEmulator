/*
 * tinycc_demo.c — TinyCC JIT example for the RV32I emulator.
 *
 * Embeds TinyCC (libtcc.c, ONE_SOURCE) compiled for RV32I (TCC_TARGET_RISCV32).
 * At runtime inside the emulator, JIT-compiles three functions from a C string:
 *   - fib(n)         : recursive Fibonacci
 *   - count_primes(n): Sieve of Eratosthenes
 *   - mandelbrot()   : ASCII Mandelbrot set renderer using fixed-point integers
 *
 * External symbols (printf, putchar) are wired in via tcc_add_symbol so the
 * JIT'd code can call into the emulator's own libc without any shared library.
 *
 * Build: see Examples/TinyCC/Program.cs for the clang invocation.
 * Run:   the emulator has no M extension; TCC lowers '*', '/' and '%'
 *        to software helper libcalls (__mulsi3/__divsi3/...).
 */

/* ── Configuration for TinyCC internals ─────────────────────────────── */
#define ONE_SOURCE          1
#define TCC_TARGET_RISCV32  1

/* Pull in all TCC sources in one translation unit */
#include "../tinycc/libtcc.c"

/* ── JIT source lives at a fixed RAM address written by the C# host ─────
 *
 * Program.cs writes the C source string into emulator RAM at JIT_SRC_ADDR
 * before execution starts. This pointer just references that location.
 * The source is null-terminated; no copy is needed.                       */
#define JIT_SRC_ADDR 0x00D00000
static const char *jit_src = (const char *)JIT_SRC_ADDR;

/* ── Demo entry point ────────────────────────────────────────────────── */

typedef int  (*fib_fn)(int);
typedef int  (*count_primes_fn)(int);
typedef void (*mandelbrot_fn)(int, int);

static TCCState *compile_jit(void)
{
    TCCState *s = tcc_new();
    if (!s) { printf("ERROR: tcc_new() failed\n"); return 0; }

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_options(s, "-nostdlib -nostdinc");

    /* Wire host runtime symbols into the JIT'd code */
    tcc_add_symbol(s, "printf",  (void *)printf);
    tcc_add_symbol(s, "putchar", (void *)putchar);

    /* The RV32I emulator implements the bare base ISA with NO M
     * extension: hardware MUL/MULH* and DIV/DIVU/REM/REMU all trap as
     * illegal instructions. The RV32 code generator therefore lowers
     * JIT'd '*', '/' and '%' to calls into these software arithmetic
     * helpers — wire them in so the JIT'd code (e.g. mandelbrot's
     * fixed-point multiplies and divides) can resolve them. The helpers
     * live in the host runtime (Runtime/runtime.c). __muldi3 covers
     * 64-bit (long long) multiply. */
    {
        extern unsigned int       __mulsi3(unsigned int, unsigned int);
        extern unsigned long long __muldi3(unsigned long long, unsigned long long);
        extern unsigned int __udivsi3(unsigned int, unsigned int);
        extern unsigned int __umodsi3(unsigned int, unsigned int);
        extern int          __divsi3(int, int);
        extern int          __modsi3(int, int);
        tcc_add_symbol(s, "__mulsi3",  (void *)__mulsi3);
        tcc_add_symbol(s, "__muldi3",  (void *)__muldi3);
        tcc_add_symbol(s, "__divsi3",  (void *)__divsi3);
        tcc_add_symbol(s, "__udivsi3", (void *)__udivsi3);
        tcc_add_symbol(s, "__modsi3",  (void *)__modsi3);
        tcc_add_symbol(s, "__umodsi3", (void *)__umodsi3);
    }

    if (tcc_compile_string(s, jit_src) != 0) {
        printf("ERROR: tcc_compile_string() failed\n");
        tcc_delete(s); return 0;
    }
    if (tcc_relocate(s) < 0) {
        printf("ERROR: tcc_relocate() failed\n");
        tcc_delete(s); return 0;
    }
    return s;
}

void _start(void)
{
    int i;

    printf("=== TinyCC JIT demo on RV32I ===\n\n");
    printf("Source code loaded from host memory at 0x%08X:\n", JIT_SRC_ADDR);
    printf("─────────────────────────────────────────\n");
    printf("%s", jit_src);
    printf("─────────────────────────────────────────\n\n");
    printf("Compiling with TinyCC (JIT, in-memory)...\n");

    TCCState *s = compile_jit();
    if (!s) exit(1);

    fib_fn         fib         = (fib_fn)        tcc_get_symbol(s, "fib");
    count_primes_fn count_primes= (count_primes_fn)tcc_get_symbol(s, "count_primes");
    mandelbrot_fn  mandelbrot  = (mandelbrot_fn) tcc_get_symbol(s, "mandelbrot");

    if (!fib || !count_primes || !mandelbrot) {
        printf("ERROR: tcc_get_symbol() failed\n");
        tcc_delete(s); exit(1);
    }

    /* ── 1. Recursive Fibonacci ──────────────────────────────────────── */
    printf("Fibonacci(0..12): ");
    for (i = 0; i <= 12; i++)
        printf("%d ", fib(i));
    printf("\n");

    if (fib(10) != 55) {
        printf("FAIL: fib(10) expected 55, got %d\n", fib(10));
        tcc_delete(s); exit(1);
    }

    /* ── 2. Sieve of Eratosthenes ────────────────────────────────────── */
    int p100  = count_primes(100);
    int p1000 = count_primes(1000);
    printf("Primes up to  100: %d  (expected 25)\n",  p100);
    printf("Primes up to 1000: %d  (expected 168)\n", p1000);

    if (p100 != 25 || p1000 != 168) {
        printf("FAIL: prime counts wrong\n");
        tcc_delete(s); exit(1);
    }

    /* ── 3. Mandelbrot ASCII art (floating-point JIT code) ───────────── */
    printf("\nMandelbrot set (60x22, rendered by JIT'd RV32I code):\n");
    mandelbrot(60, 22);

    printf("\nSUCCESS: all JIT results correct!\n");
    tcc_delete(s);
    exit(0);
}
