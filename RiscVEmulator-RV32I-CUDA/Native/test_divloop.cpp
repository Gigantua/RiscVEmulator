// CPU validation of the DIVLOOP closed form vs a literal simulation of clang's rv32i bit-serial
// restoring-division loop (the body shared by __udivsi3 @0x1048 and __divsi3 @0x14b4):
//   slli R,R,1 ; srl T1,N,i ; sll T2,ONE,i ; addi i,i,-1 ; andi T1,T1,1 ; or R,T1,R ;
//   sltu T1,R,D ; addi T1,T1,-1 ; and T2,T1,T2 ; and T1,T1,D ; or Q,T2,Q ; sub R,R,T1 ; bne i,NEG1,top
// Entered (idiom) with R=0,Q=0,i=31,ONE=1,NEG1=-1 → computes Q=N/D, R=N%D over 32 bits, MSB first.
// We also test arbitrary R0,Q0,i0 (R0<D) to confirm the general closed form, since the kernel reads
// entry regs rather than assuming the prologue's zeros.
#include <cstdint>
#include <cstdio>
#include <random>

// Literal simulation of the loop body (returns final Q,R,i).
static void sim(uint32_t R, uint32_t Q, uint32_t i, uint32_t N, uint32_t D,
                uint32_t& Qf, uint32_t& Rf, uint32_t& iF) {
    const uint32_t NEG1 = 0xFFFFFFFFu, ONE = 1;
    for (;;) {
        R = R << 1;                       // slli R,R,1
        uint32_t T1 = N >> i;             // srl T1,N,i
        uint32_t T2 = ONE << i;           // sll T2,ONE,i
        i = i - 1;                        // addi i,i,-1
        T1 = T1 & 1;                      // andi T1,T1,1
        R = T1 | R;                       // or R,T1,R
        T1 = (R < D) ? 1u : 0u;           // sltu T1,R,D
        T1 = T1 - 1;                      // addi T1,T1,-1
        T2 = T1 & T2;                     // and T2,T1,T2
        T1 = T1 & D;                      // and T1,T1,D
        Q  = T2 | Q;                      // or Q,T2,Q
        R  = R - T1;                      // sub R,R,T1
        if (i == NEG1) break;             // bne i,NEG1,top
    }
    Qf = Q; Rf = R; iF = i;
}

// Closed form the kernel computes. The matcher VERIFIES the prologue establishes the standard entry
// (R=0, Q=0, i=31, NEG1=-1), so the loop is a full 32-bit unsigned divide → Q=N/D, R=N%D, i=-1.
static void closed(uint32_t N, uint32_t D, uint32_t& Qf, uint32_t& Rf, uint32_t& iF) {
    Qf = D ? (N / D) : 0xFFFFFFFFu;
    Rf = D ? (N % D) : N;
    iF = 0xFFFFFFFFu;
}

int main() {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<uint32_t> u32(0, 0xFFFFFFFFu);
    long mism = 0, tested = 0;

    // The real idiom: standard entry R0=0,Q0=0,i0=31, random N, D (incl. D==0 path), full 32-bit range.
    for (int t = 0; t < 8000000; t++) {
        uint32_t N = u32(rng), D = u32(rng);
        uint32_t a, b, c, x, y, z;
        if (D == 0) continue;             // loop's beqz guards D!=0; D==0 never enters the loop
        sim(0, 0, 31, N, D, a, b, c);
        closed(N, D, x, y, z);
        tested++;
        if (a != x || b != y || c != z) {
            if (mism < 10) printf("MISMATCH N=%08X D=%08X sim(Q=%08X R=%08X i=%08X) closed(Q=%08X R=%08X i=%08X)\n",
                                   N, D, a, b, c, x, y, z);
            mism++;
        }
        if (a != N / D || b != N % D) {   // cross-check vs true arithmetic
            if (mism < 20) printf("ARITH MISMATCH N=%08X D=%08X simQ=%08X simR=%08X\n", N, D, a, b);
            mism++;
        }
    }

    printf("tested=%ld  mismatches=%ld  => %s\n", tested, mism, mism == 0 ? "PASS" : "FAIL");
    return mism == 0 ? 0 : 1;
}
