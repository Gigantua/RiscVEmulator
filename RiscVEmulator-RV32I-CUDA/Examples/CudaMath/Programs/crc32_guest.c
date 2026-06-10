// crc32_guest.c — table-less bitwise CRC-32 (poly 0xEDB88320) over an LCG-filled 8 KB buffer,
// 64 passes (bit-twiddling + byte loads). Result must equal the C# mirror. Halts via ebreak.
#include <stdint.h>

static uint8_t buf[8192];

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    uint32_t x = 0xC0FFEE01u;                       // xorshift32: multiply-free fill
    for (uint32_t i = 0; i < 8192u; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (uint8_t)(x >> 24);
    }
    uint32_t acc = 0;
    for (uint32_t rep = 0; rep < 64u; rep++) {
        uint32_t crc = 0xFFFFFFFFu ^ rep;
        for (uint32_t i = 0; i < 8192u; i++) {
            crc ^= buf[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        acc = acc * 33u + ~crc;
    }
    *out = acc;
    __asm__ volatile ("ebreak");
}
