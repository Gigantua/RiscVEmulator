/* bench_crc.c — CRC-32 over a buffer.
 *
 * Pure ALU: shifts, xor, and. Tight inner loop with predictable branches.
 * Pleasant case for any JIT — most operations have a 1:1 mapping to native
 * x86. Used to measure JIT body throughput in the absence of MMIO.
 */

#include "libc.h"

#define BUF_SIZE  16384u
#define ITERS     500u

static unsigned char buf[BUF_SIZE];

void _start(void)
{
    for (unsigned int i = 0; i < BUF_SIZE; i++)
        buf[i] = (unsigned char)((i * 31u) ^ (i >> 5));

    unsigned int crc = 0xFFFFFFFFu;
    for (unsigned int it = 0; it < ITERS; it++) {
        unsigned int c = crc;
        for (unsigned int i = 0; i < BUF_SIZE; i++) {
            c ^= buf[i];
            for (int b = 0; b < 8; b++) {
                unsigned int mask = 0u - (c & 1u);
                c = (c >> 1) ^ (0xEDB88320u & mask);
            }
        }
        crc = c;
    }

    printf("crc=0x%08x\n", crc);
    exit(0);
}
