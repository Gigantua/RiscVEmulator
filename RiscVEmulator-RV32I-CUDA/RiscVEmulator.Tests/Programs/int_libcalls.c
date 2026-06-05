/* int_libcalls.c — exercise every compiler-rt integer libcall that
 * Runtime/runtime.c provides, on edge-case inputs. Prints one line per
 * op as hex. A host-side test ('IntLibcallsTest.cs') runs the same
 * operations on the host and asserts byte-for-byte agreement.
 *
 * If any line diverges, we have a runtime.c bug.
 */

#include <stdint.h>

static void puthex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    volatile char *uart = (volatile char *)0x10000000;
    for (int i = 7; i >= 0; i--) *uart = hex[(v >> (i * 4)) & 0xF];
}
static void puthex64(uint64_t v) { puthex32((uint32_t)(v >> 32)); puthex32((uint32_t)v); }
static void putc_(char c) { *(volatile char *)0x10000000 = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* Force the compiler to emit libcalls instead of inlining (volatiles
 * defeat constant-folding). */
static volatile uint32_t u32_in[16] = {
    0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFu, 0xFFFFu,
    0x10000u, 0x12345678u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
    0xFFFFFFFEu, 0xFFFFFFFFu, 0xDEADBEEFu,
};
static volatile int32_t s32_in[16] = {
    0, 1, -1, 2, -2, 127, -128, 0x7FFF,
    -0x7FFF - 1, 0x12345, -0x12345, 0x7FFFFFFF,
    (int32_t)0x80000000, (int32_t)0x80000001, -0x10000, 0x10000,
};
static volatile uint64_t u64_in[8] = {
    0ULL, 1ULL, 0xFFFFFFFFULL, 0x100000000ULL,
    0x123456789ABCDEFULL, 0x8000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL, 0xDEADBEEFCAFEBABEULL,
};
static volatile int64_t s64_in[8] = {
    0LL, 1LL, -1LL, 0x7FFFFFFFFFFFFFFFLL,
    (int64_t)0x8000000000000000LL, 12345678901234LL,
    -12345678901234LL, 0xCAFEBABEDEADBEEFLL,
};

void _start(void) {
    /* 32-bit multiply */
    puts_("MUL32\n");
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        uint32_t a = u32_in[i], b = u32_in[j];
        puthex32(a * b); putc_('\n');
    }
    /* 32-bit unsigned divide / mod */
    puts_("UDIV32\n");
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        uint32_t a = u32_in[i], b = u32_in[j];
        puthex32(b ? a / b : 0xFFFFFFFFu); putc_('\n');
        puthex32(b ? a % b : a); putc_('\n');
    }
    /* 32-bit signed divide / mod */
    puts_("SDIV32\n");
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        int32_t a = s32_in[i], b = s32_in[j];
        /* Match RISC-V DIV/REM spec for /0 and INT_MIN/-1 */
        int32_t q, r;
        if (b == 0) { q = -1; r = a; }
        else if (a == (int32_t)0x80000000 && b == -1) { q = a; r = 0; }
        else { q = a / b; r = a % b; }
        puthex32((uint32_t)q); putc_('\n');
        puthex32((uint32_t)r); putc_('\n');
    }
    /* 64-bit multiply */
    puts_("MUL64\n");
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        uint64_t a = u64_in[i], b = u64_in[j];
        puthex64(a * b); putc_('\n');
    }
    /* 64-bit unsigned divide / mod */
    puts_("UDIV64\n");
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        uint64_t a = u64_in[i], b = u64_in[j];
        puthex64(b ? a / b : 0xFFFFFFFFFFFFFFFFULL); putc_('\n');
        puthex64(b ? a % b : a); putc_('\n');
    }
    /* 64-bit signed divide / mod */
    puts_("SDIV64\n");
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        int64_t a = s64_in[i], b = s64_in[j];
        int64_t q, r;
        if (b == 0) { q = -1; r = a; }
        else if (a == (int64_t)0x8000000000000000LL && b == -1) { q = a; r = 0; }
        else { q = a / b; r = a % b; }
        puthex64((uint64_t)q); putc_('\n');
        puthex64((uint64_t)r); putc_('\n');
    }
    /* 64-bit shifts */
    puts_("SHL64\n");
    for (int i = 0; i < 8; i++) for (int s = 0; s < 64; s++) {
        uint64_t a = u64_in[i];
        puthex64(a << s); putc_('\n');
    }
    puts_("SHR64\n");
    for (int i = 0; i < 8; i++) for (int s = 0; s < 64; s++) {
        uint64_t a = u64_in[i];
        puthex64(a >> s); putc_('\n');
    }
    puts_("SAR64\n");
    for (int i = 0; i < 8; i++) for (int s = 0; s < 64; s++) {
        int64_t a = s64_in[i];
        puthex64((uint64_t)(a >> s)); putc_('\n');
    }
    puts_("DONE\n");
    *(volatile uint32_t *)0x40000000 = 0;
    for (;;) {}
}
