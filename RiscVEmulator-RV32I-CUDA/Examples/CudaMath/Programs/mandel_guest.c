// mandel_guest.c — Mandelbrot in 16.16 fixed point over a 96×64 grid, max 64 iterations.
// rv32i has no hardware multiply, so the 32×32→64 product is an explicit shift-add loop
// (the classic soft-mul a math-heavy rv32i program actually runs). Result = FNV over the
// per-pixel iteration counts — must equal the C# mirror, which transcribes this code
// EXACTLY (sign-magnitude fixed multiply, same rounding). Halts via ebreak.
#include <stdint.h>

// optnone: keeps this a real shift-add loop (LLVM can rewrite multiply-shaped loops into
// `mul`, which lowers to an unlinkable __mulsi3 libcall under rv32i -nostdlib).
__attribute__((optnone)) static uint32_t fixmul_mag(uint32_t a, uint32_t b) {   // (a*b) >> 16, exact 64-bit product
    uint32_t lh = 0, ll = 0, ah = 0, al = a;
    while (b) {
        if (b & 1u) { uint32_t t = ll + al; lh += ah + (t < ll); ll = t; }
        ah = (ah << 1) | (al >> 31); al <<= 1;
        b >>= 1;
    }
    return (ll >> 16) | (lh << 16);
}
static int32_t fmul(int32_t a, int32_t b) {            // signed 16.16 multiply, sign-magnitude
    uint32_t ua = a < 0 ? (uint32_t)-a : (uint32_t)a;
    uint32_t ub = b < 0 ? (uint32_t)-b : (uint32_t)b;
    uint32_t m  = fixmul_mag(ua, ub);
    return ((a < 0) != (b < 0)) ? -(int32_t)m : (int32_t)m;
}

void _start(void) {
    volatile uint32_t* out = (volatile uint32_t*)0x7000u;
    const int32_t x0 = -(2 << 16) - (1 << 15);         // -2.5
    const int32_t y0 = -(1 << 16);                     // -1.0
    const int32_t dx = (3 << 16) / 96 + ((1 << 15) / 96);   // 3.5 / 96
    const int32_t dy = (2 << 16) / 64;                 // 2.0 / 64
    uint32_t h = 0x811C9DC5u;
    for (int py = 0; py < 64; py++) {
        int32_t ci = y0 + dy * py;
        for (int px = 0; px < 96; px++) {
            int32_t cr = x0 + dx * px;
            int32_t zr = 0, zi = 0;
            uint32_t it = 0;
            while (it < 64u) {
                int32_t zr2 = fmul(zr, zr), zi2 = fmul(zi, zi);
                if (zr2 + zi2 > (4 << 16)) break;
                int32_t nzr = zr2 - zi2 + cr;
                zi = 2 * fmul(zr, zi) + ci;
                zr = nzr;
                it++;
            }
            h = (h ^ it) * 16777619u;
        }
    }
    *out = h;
    __asm__ volatile ("ebreak");
}
