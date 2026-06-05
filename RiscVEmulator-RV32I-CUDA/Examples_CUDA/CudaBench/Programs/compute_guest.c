// compute_guest.c — fetch-bound RV32I micro-benchmark. A tiny register-only
// inner loop (no data loads) that fits in a single 32-instruction prefetch
// section, so it isolates instruction-fetch latency: with the shared
// instruction window the fetch is essentially free, without it every
// instruction pays the full ~530-cycle dependent-load latency.

#include <stdint.h>

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)(uintptr_t)0x00003000u;  // result sink (just above code)
    uint32_t x = 12345u;
    for (;;) {
        for (int i = 0; i < 4096; i++) {
            x = x * 1664525u + 1013904223u;   // LCG
            x ^= x >> 15;
            x += 0x9e3779b9u;
        }
        *out = x;                              // one store per 4096 compute iters
    }
}
