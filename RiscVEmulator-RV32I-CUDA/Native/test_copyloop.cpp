// CPU-only correctness test for the COPYLOOP word-widening algorithm (no CUDA, no GPU).
// Compares the exact in-kernel COPYLOOP copy against the trivial byte-by-byte RV32I loop it
// replaces, across every length and src/dst overlap distance. The byte loop IS the ground truth
// (it's literally what `lb;sb;addi;addi;bne` does), so any mismatch is a real miscompile.
#include <cstdint>
#include <cstdio>
#include <cstring>

static uint32_t ld32(const uint8_t* m, uint32_t a){ uint32_t v; memcpy(&v, m + a, 4); return v; }
static void     st32(uint8_t* m, uint32_t a, uint32_t v){ memcpy(m + a, &v, 4); }

// Faithful copy of the RC_COPYLOOP kernel arm (memory effects only).
static void copyloop(uint8_t* m, uint32_t s, uint32_t d, uint32_t L, bool cdst){
    do {
        uint32_t cnt = cdst ? d : s, rem = L - cnt, adiff = (d > s) ? (d - s) : (s - d);
        if (rem >= 4u && adiff >= 4u) { st32(m, d, ld32(m, s)); s += 4; d += 4; }
        else                         { m[d] = m[s];            s += 1; d += 1; }
    } while ((cdst ? d : s) != L);
}
// Ground truth: the exact byte-granular do-while loop the guest runs.
static void refcopy(uint8_t* m, uint32_t s, uint32_t d, uint32_t L, bool cdst){
    do { m[d] = m[s]; s += 1; d += 1; } while ((cdst ? d : s) != L);
}

int main(){
    int tests = 0, fails = 0;
    for (int len = 1; len <= 64; len++)
      for (int off = -32; off <= 32; off++) {
        if (off == 0) continue;                    // src==dst is excluded by the translator
        for (int cdst = 0; cdst < 2; cdst++) {
            static uint8_t a[8192], b[8192];
            for (int i = 0; i < 8192; i++) a[i] = b[i] = (uint8_t)(i * 31 + 7);
            uint32_t s = 4000, d = (uint32_t)(4000 + off);
            uint32_t L = cdst ? (d + len) : (s + len);
            copyloop(a, s, d, L, cdst != 0);
            refcopy (b, s, d, L, cdst != 0);
            tests++;
            if (memcmp(a, b, 8192) != 0) { fails++; if (fails <= 12) printf("FAIL len=%d off=%d cdst=%d\n", len, off, cdst); }
        }
      }
    printf("COPYLOOP algorithm: %d configs tested, %d mismatches vs byte-copy\n", tests, fails);
    return fails ? 1 : 0;
}
