// fib_guest.c — 50M iterations of the Fibonacci recurrence mod 2^32 (pure dependent ALU chain;
// the tightest possible serial integer loop). Result must equal the C# mirror. Halts via ebreak.
#include <stdint.h>

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    uint32_t a = 0, b = 1;
    for (uint32_t i = 0; i < 50000000u; i++) {
        uint32_t t = a + b;
        a = b;
        b = t;
    }
    *out = a;
    __asm__ volatile ("ebreak");
}
