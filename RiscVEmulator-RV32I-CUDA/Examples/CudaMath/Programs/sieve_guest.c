// sieve_guest.c — Sieve of Eratosthenes below 65536, repeated 20 times (memory + branch bound).
// Result: (prime count) ^ (rep counter mix) — must equal the C# mirror exactly. Halts via ebreak.
#include <stdint.h>

static uint32_t bits[2048];                       // 65536 bits

// optnone: stops LLVM recognizing the loop as a multiplication (→ __mulsi3, unlinkable here).
__attribute__((optnone)) static uint32_t mulu(uint32_t a, uint32_t b) {   // p*p without libgcc
    uint32_t r = 0;
    while (b) { if (b & 1u) r += a; a <<= 1; b >>= 1; }
    return r;
}

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    uint32_t acc = 0;
    for (uint32_t rep = 0; rep < 20u; rep++) {
        for (uint32_t i = 0; i < 2048u; i++) bits[i] = 0;
        for (uint32_t p = 2; p < 256u; p++) {
            if (bits[p >> 5] & (1u << (p & 31u))) continue;
            for (uint32_t m = mulu(p, p); m < 65536u; m += p)
                bits[m >> 5] |= 1u << (m & 31u);
        }
        uint32_t cnt = 0;
        for (uint32_t n = 2; n < 65536u; n++)
            if (!(bits[n >> 5] & (1u << (n & 31u)))) cnt++;
        acc = acc * 31u + cnt + rep;
    }
    *out = acc;                                    // C# mirror computes the same
    __asm__ volatile ("ebreak");
}
