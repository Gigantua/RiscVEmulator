// CPU-only validation that a hardware multiply can EXACTLY replicate the shift-add software-multiply
// loop Doom's renderer runs (profiled hot at pc 0x01634), including every live-out register and the
// exact iteration count (needed for the gate-exact weight). No CUDA, no GPU.
//
// The loop (registers generalized as M=multiplier, B=multiplicand, A=temp, ACC=accumulator):
//   do { A = M<<31; M = M>>1; A = (int32)A >> 31; A = A & B; ACC = A + ACC; B = B<<1; } while (M != 0);
// computes ACC += B*M over the bits of M (LSB-first), B shifting left.
#include <cstdint>
#include <cstdio>

static void refloop(uint32_t& A, uint32_t& B, uint32_t& M, uint32_t& ACC){
    do {
        A   = M << 31;
        M   = M >> 1;
        A   = (uint32_t)((int32_t)A >> 31);
        A   = A & B;
        ACC = A + ACC;
        B   = B << 1;
    } while (M != 0);
}

// Closed-form replacement (what the MULLOOP uop kernel would compute):
static unsigned mulloop_iters(uint32_t M0){ if(!M0) return 1u; unsigned bl=0; while(M0){bl++;M0>>=1;} return bl; }
static void fused(uint32_t& A, uint32_t& B, uint32_t& M, uint32_t& ACC){
    uint32_t B0 = B, M0 = M, ACC0 = ACC;
    unsigned it = mulloop_iters(M0);
    ACC = ACC0 + B0 * M0;                          // accumulate the product (mod 2^32)
    M   = 0;                                        // multiplier shifted to 0
    B   = (it >= 32) ? 0u : (B0 << it);             // multiplicand shifted left once per iteration
    A   = (M0 == 0) ? 0u : ((it - 1 >= 32) ? 0u : (B0 << (it - 1)));  // last-iteration mask & B
}

int main(){
    int tests = 0, fails = 0;
    // exhaustive-ish over multiplier bit patterns + sampled multiplicand/accumulator
    uint32_t Ms[] = {0,1,2,3,5,7,8,15,16,255,256,0x7FFF,0x8000,0xFFFF,0x10000,0x7FFFFFFF,0x80000000,0xFFFFFFFF,0xABCDEF01,0x00010000};
    uint32_t Bs[] = {0,1,2,7,0xFF,0x1234,0x80000000,0xFFFFFFFF,0xDEADBEEF,3};
    uint32_t Cs[] = {0,1,0x12345678,0xFFFFFFFF};
    for (uint32_t M : Ms) for (uint32_t B : Bs) for (uint32_t C : Cs) {
        uint32_t a1=0,b1=B,m1=M,c1=C;  refloop(a1,b1,m1,c1);
        uint32_t a2=0,b2=B,m2=M,c2=C;  fused (a2,b2,m2,c2);
        tests++;
        if (a1!=a2 || b1!=b2 || m1!=m2 || c1!=c2) { fails++;
            if (fails<=12) printf("FAIL M=%08X B=%08X C=%08X | ref A=%08X B=%08X M=%08X ACC=%08X | fused A=%08X B=%08X M=%08X ACC=%08X\n",
                M,B,C, a1,b1,m1,c1, a2,b2,m2,c2); }
    }
    printf("MULLOOP semantics: %d configs, %d mismatches (regs A,B,M,ACC)\n", tests, fails);
    return fails ? 1 : 0;
}
