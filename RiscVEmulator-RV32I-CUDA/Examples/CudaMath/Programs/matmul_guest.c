// matmul_guest.c — 24×24 uint32 matrix multiply with an explicit shift-add soft multiply
// (rv32i has no M extension), 32 rounds with the product fed back into A. Result = FNV over
// the final matrix — must equal the C# mirror. Halts via ebreak.
#include <stdint.h>

#define N 24
static uint32_t A[N*N], B[N*N], C[N*N];

// optnone: LLVM otherwise recognizes the shift-add loop AS a multiplication and emits
// `mul` — which lowers to a __mulsi3 libcall rv32i -nostdlib cannot link.
__attribute__((optnone)) static uint32_t mulu(uint32_t a, uint32_t b) {   // 32×32→32 low product
    uint32_t r = 0;
    while (b) { if (b & 1u) r += a; a <<= 1; b >>= 1; }
    return r;
}

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    uint32_t x = 0x12345678u;                       // xorshift32: multiply-free fill
    for (int i = 0; i < N*N; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; A[i] = x >> 16;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; B[i] = x >> 16;
    }
    for (int rep = 0; rep < 32; rep++) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                uint32_t s = 0;
                for (int k = 0; k < N; k++)
                    s += mulu(A[i*N+k], B[k*N+j]);
                C[i*N+j] = s;
            }
        for (int i = 0; i < N*N; i++) A[i] = (C[i] >> 7) | 1u;   // feed back, keep operands small-ish
    }
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < N*N; i++) h = (h ^ C[i]) * 16777619u;
    *out = h;
    __asm__ volatile ("ebreak");
}
