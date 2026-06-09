// divergent_guest.c — a realistic, control-flow-DIVERGENT RV32I guest.
//
// Unlike compute_guest / bench_guest (identical code + data on every core, so a
// warp's 32 lanes march in perfect lockstep), this guest is seeded per-core: the
// host writes the core index at SEED_ADDR before launch. Every lane therefore
// runs a different pseudo-random stream, so the data-dependent branch and the
// 8-way switch below resolve differently across the warp — the lanes diverge and
// the hardware serializes the taken paths. That is the regime real guests (DOOM,
// an OS, an interpreter) actually run in: branchy, indirection-heavy, memory-
// touching code, not a tight uniform ALU loop. Aggregate MIPS here is the honest
// "jumpy code under divergence" number.
//
// Pure RV32I: only constant multiplies (strength-reduced to shift/add — no M
// extension, no libcalls), xorshift PRNG, branches, a switch, and array
// load/modify/store. Never halts; the host caps it with a step budget.

#include <stdint.h>

#define SEED_ADDR  0x00000040u    // host writes the per-core seed (core index) here
#define OUT_ADDR   0x00000080u    // result sink (host may read it back)
#define ARR_ADDR   0x00000100u    // small working array, below the 0x1000 code base
#define ARR_WORDS  256u           // power of two → cheap index mask

void _start(void) {
    volatile uint32_t* seedp = (volatile uint32_t*)(uintptr_t)SEED_ADDR;
    volatile uint32_t* out   = (volatile uint32_t*)(uintptr_t)OUT_ADDR;
    uint32_t*          arr   = (uint32_t*)(uintptr_t)ARR_ADDR;

    uint32_t x = *seedp * 2654435761u + 0x9e3779b9u;   // distinct stream per core
    if (x == 0u) x = 1u;                               // xorshift must avoid 0
    for (uint32_t i = 0; i < ARR_WORDS; i++) { x += 0x6d2b79f5u; arr[i] = x; }

    uint32_t acc = 0u;
    for (;;) {
        for (uint32_t i = 0; i < 4096u; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;   // xorshift32 (branchless)

            uint32_t idx = x & (ARR_WORDS - 1u);
            uint32_t v   = arr[idx];                   // data-dependent load

            switch ((x >> 8) & 7u) {                   // 8-way dispatch — diverges per core
                case 0:  v += acc;                 break;
                case 1:  v ^= x;                   break;
                case 2:  v -= (x >> 3);            break;
                case 3:  v = (v << 1) | (v >> 31); break;   // rotate
                case 4:  v += x * 9u;              break;   // const mul → shift/add
                case 5:  v &= (x | 1u);            break;
                case 6:  v ^= (v >> 7);            break;
                default: v += 0x1234u;             break;
            }

            if ((x & 0x10000u) != 0u) acc += v;        // data-dependent branch — diverges
            else                      acc ^= (v + i);

            arr[idx] = v;                              // store
        }
        *out = acc;                                    // periodic result
    }
}
