// jit_test.c — finite, deterministic, self-halting guest for JIT validation.
// Does a fixed amount of LCG work, writes the result to the sink, then halts
// via the host-exit device. Both the interpreter and the JIT run it to halt
// from the same initial state, so their final sink must match bit-for-bit; the
// wall-time ratio is the JIT speedup. Counted (finite) so each run is short and
// can't trip the TDR watchdog.

#include <stdint.h>

void _start(void) {
    volatile uint32_t* out  = (volatile uint32_t*)(uintptr_t)0x00003000u;
    volatile uint32_t* exit = (volatile uint32_t*)(uintptr_t)0x40000000u;
    uint32_t x = 12345u;
    for (uint32_t k = 0; k < 100000u; k++) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 15;
        x += 0x9e3779b9u;
    }
    *out  = x;     // result
    *exit = 0;     // halt (host-exit device)
    for (;;) {}    // unreachable
}
