// bench_guest.c — freestanding RV32IMA throughput micro-benchmark guest.
// A never-halting loop with a representative mix: a multiply, two adds, a load
// and a store per inner iteration over a small array (so it exercises the M
// extension and the memory pipeline). Each GPU core runs its own copy on its
// own RAM; the host runs every core for a fixed step budget and divides
// (cores * budget) by wall time to get aggregate MIPS.

#include <stdint.h>

void _start(void) {
    volatile uint32_t* a = (volatile uint32_t*)(uintptr_t)0x00004000u;  // 4 KiB working set
    uint32_t x = 12345u;
    for (;;) {
        for (int i = 0; i < 1024; i++) {
            x = a[i] * 1664525u + 1013904223u + x;   // LCG: mul + add + add
            a[i] = x;                                  // store
        }
    }
}
