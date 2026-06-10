// collatz_guest.c — total Collatz trajectory steps for every n in 1..100000 (add/shift/branch
// bound, highly data-dependent control flow; all intermediates fit u32 for this range).
// Result must equal the C# mirror. Halts via ebreak.
#include <stdint.h>

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    uint32_t total = 0;
    for (uint32_t n = 1; n <= 100000u; n++) {
        uint32_t v = n;
        while (v != 1u) {
            v = (v & 1u) ? 3u * v + 1u : v >> 1;
            total++;
        }
    }
    *out = total;
    __asm__ volatile ("ebreak");
}
