// verify_guest.c — multi-core differential-verification guest. Reads a per-core seed,
// runs a deterministic mix of ALU, data-dependent branches, and aligned AND misaligned
// memory traffic over an 8 KB scratch region, writes a final hash to the result sink,
// then HALTS (ebreak). Because it runs to completion, two emulator backends can be
// compared per core without retired-count bookkeeping: final state is total.
#include <stdint.h>

void _start(void) {
    volatile uint32_t* seedp = (volatile uint32_t*)0x40u;     // per-core seed (host-written)
    volatile uint32_t* out   = (volatile uint32_t*)0x3000u;   // result sink
    uint8_t* arr = (uint8_t*)0x4000u;                         // 8 KB scratch (0x4000..0x6000)

    uint32_t x = *seedp * 2654435761u + 1u;
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < 60000u; i++) {
        x = x * 1664525u + 1013904223u;                       // LCG
        x ^= x >> 13;
        uint32_t off = x & 0x1FFCu;                           // word-aligned scratch offset
        *(volatile uint32_t*)(arr + off) = x ^ i;             // aligned sw
        h += *(volatile uint32_t*)(arr + off);                // aligned lw
        uint32_t moff = (x >> 3) & 0x1FF8u;                   // 8-aligned base for the unaligned ops
        *(volatile uint8_t*)(arr + moff + (x & 3u)) = (uint8_t)x;     // sb at arbitrary byte
        h ^= *(volatile uint32_t*)(arr + moff + 1u);                  // MISALIGNED lw (odd address)
        *(volatile uint16_t*)(arr + moff + 2u) = (uint16_t)h;         // sh (2-aligned)
        h += *(volatile uint16_t*)(arr + moff + 4u);                  // lh
        h ^= *(volatile uint16_t*)(arr + moff + 3u);                  // MISALIGNED lh
        if (x & 0x8000u) h = (h << 1) | (h >> 31);            // data-dependent branch
        else             h ^= x + i;
    }
    *out = h;
    __asm__ volatile ("ebreak");
}
