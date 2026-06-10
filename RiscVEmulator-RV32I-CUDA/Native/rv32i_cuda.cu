#ifndef RVCUD_HOTHIST       // profiling only: count per-uop executions on core 0, dump CSV (ui,pc,class,count) on shutdown
#define RVCUD_HOTHIST 0
#endif

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <atomic>
#include <cuda_runtime.h>
#include <cuda.h>            // driver API (cuModuleLoadDataEx / cuLaunchKernel) — for the tiered exec_block cross-compiler
#ifdef _WIN32                // peak-commit reporting for the exec_block build (ptxas memory-knee watch)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

static constexpr uint32_t HALT_BIT = 0x80000000u;

struct CoreState { uint32_t regs[32], pc; };

// Per-core RAM is stored WORD-INTERLEAVED across cores: guest word w of core
// `id` lives at mem[(size_t)w*nc + id]. Consecutive lanes in a warp touching the
// same guest address hit consecutive words → one coalesced 128 B transaction.
// Sub-word access reads/masks the containing word(s); a core owns its own words,
// so the read-modify-write on a store has no cross-lane race.
// Software prefetch of a guest byte address into L2 (hint; non-blocking). Used to warm the next
// loop iteration's source while the current one computes — hides global latency on a single warp.
static __device__ __forceinline__ void pf_i(const uint32_t* m, int nc, int id, uint32_t a) {
    const uint32_t* p = m + (size_t)(a >> 2) * nc + id;
    asm volatile("prefetch.global.L2 [%0];" :: "l"(p));
}
// Same hint for a HOST-array address (uop descriptors): issued once at fused-loop entry so the
// loop-exit dispatch's cold uops[ft]/uw[ft] loads are already in L2 when the loop finishes.
static __device__ __forceinline__ void pf_h(const void* p) {
    asm volatile("prefetch.global.L2 [%0];" :: "l"(p));
}
// NC1 (compile-time): single-core layout — the interleave degenerates to a flat linear array, so guest
// byte address a lives at ((uint8_t*)mem)[a] exactly. Bytes are direct loads/stores (no word RMW);
// half/word keep an aligned fast path (misaligned ld.u32/u16 faults on GPU) + byte-assembled fallback.
template<class T, bool NC1> static __device__ __forceinline__ T ld_i(const uint32_t* m, int nc, int id, uint32_t a) {
    if constexpr (NC1) {
        const uint8_t* b = (const uint8_t*)m;
        if constexpr (sizeof(T) == 4) {
            if ((a & 3u) == 0) return (T)*(const uint32_t*)(b + a);
            return (T)((uint32_t)b[a] | ((uint32_t)b[a+1] << 8) | ((uint32_t)b[a+2] << 16) | ((uint32_t)b[a+3] << 24));
        } else if constexpr (sizeof(T) == 2) {
            if ((a & 1u) == 0) return (T)*(const uint16_t*)(b + a);
            return (T)((uint32_t)b[a] | ((uint32_t)b[a+1] << 8));
        } else return (T)b[a];
    }
    uint32_t w = a >> 2, off = (a & 3u) << 3;
    uint32_t lo = m[(size_t)w * nc + id];
    if constexpr (sizeof(T) == 4) {
        if (off == 0) return (T)lo;
        uint32_t hi = m[(size_t)(w + 1) * nc + id];
        return (T)((lo >> off) | (hi << (32 - off)));
    } else if constexpr (sizeof(T) == 2) {
        if (off <= 16) return (T)(lo >> off);
        uint32_t hi = m[(size_t)(w + 1) * nc + id];
        return (T)((lo >> off) | (hi << (32 - off)));
    } else return (T)(lo >> off);
}
template<class T, bool NC1> static __device__ __forceinline__ void st_i(uint32_t* m, int nc, int id, uint32_t a, T v) {
    if constexpr (NC1) {
        uint8_t* b = (uint8_t*)m;
        if constexpr (sizeof(T) == 4) {
            uint32_t vv = (uint32_t)v;
            if ((a & 3u) == 0) { *(uint32_t*)(b + a) = vv; return; }
            b[a] = (uint8_t)vv; b[a+1] = (uint8_t)(vv >> 8); b[a+2] = (uint8_t)(vv >> 16); b[a+3] = (uint8_t)(vv >> 24);
        } else if constexpr (sizeof(T) == 2) {
            uint32_t vv = (uint16_t)v;
            if ((a & 1u) == 0) { *(uint16_t*)(b + a) = (uint16_t)vv; return; }
            b[a] = (uint8_t)vv; b[a+1] = (uint8_t)(vv >> 8);
        } else b[a] = (uint8_t)v;
        return;
    }
    uint32_t w = a >> 2, off = (a & 3u) << 3;
    size_t i0 = (size_t)w * nc + id;
    if constexpr (sizeof(T) == 4) {
        if (off == 0) { m[i0] = (uint32_t)v; return; }
        size_t i1 = (size_t)(w + 1) * nc + id;
        uint32_t lomask = (1u << off) - 1u;
        m[i0] = (m[i0] & lomask)  | ((uint32_t)v << off);
        m[i1] = (m[i1] & ~lomask) | ((uint32_t)v >> (32 - off));
    } else if constexpr (sizeof(T) == 2) {
        uint32_t vv = (uint16_t)v;
        if (off <= 16) { uint32_t mask = 0xFFFFu << off; m[i0] = (m[i0] & ~mask) | (vv << off); }
        else {
            size_t i1 = (size_t)(w + 1) * nc + id;
            m[i0] = (m[i0] & 0x00FFFFFFu) | (vv << 24);
            m[i1] = (m[i1] & 0xFFFFFF00u) | (vv >> 8);
        }
    } else {
        uint32_t mask = 0xFFu << off;
        m[i0] = (m[i0] & ~mask) | ((uint32_t)(uint8_t)v << off);
    }
}

static __device__ __forceinline__ uint32_t alu(uint32_t f3, uint32_t u1, uint32_t a2u,
        uint32_t sh, bool sub, bool sra) {
    int32_t  s1 = (int32_t)u1, a2s = (int32_t)a2u;
    uint32_t add  = sub ? (u1 - a2u) : (u1 + a2u);
    uint32_t sll  = u1 << sh;
    uint32_t slt  = (uint32_t)(s1 < a2s);
    uint32_t sltu = (uint32_t)(u1 < a2u);
    uint32_t xr   = u1 ^ a2u;
    uint32_t sr   = sra ? (uint32_t)(s1 >> sh) : (u1 >> sh);
    uint32_t orr  = u1 | a2u;
    uint32_t andr = u1 & a2u;
    uint32_t e0 = (f3 & 4u) ? xr   : add;
    uint32_t e1 = (f3 & 4u) ? sr   : sll;
    uint32_t e2 = (f3 & 4u) ? orr  : slt;
    uint32_t e3 = (f3 & 4u) ? andr : sltu;
    return (f3 & 1u) ? ((f3 & 2u) ? e3 : e1) : ((f3 & 2u) ? e2 : e0);
}

// Assemble the sign-extended, opcode-appropriate immediate for one instruction. Done inline in the
// fetch path so the interpreter reads live guest RAM (correct for self-modifying / JIT guests like
// guests) rather than a stale predecoded copy.
static __device__ __forceinline__ uint32_t decode_imm(uint32_t instr) {
    const uint32_t op = instr & 0x7F;
    if (op == 0x63)                        // B-type
        return (((instr>>31)&1u)<<12 | ((instr>>7)&1u)<<11 | ((instr>>25)&0x3Fu)<<5 | ((instr>>8)&0xFu)<<1)
             | ((instr & 0x80000000u) ? 0xFFFFE000u : 0u);
    if (op == 0x6F)                        // J-type
        return (((instr>>31)&1u)<<20 | ((instr>>12)&0xFFu)<<12 | ((instr>>20)&1u)<<11 | ((instr>>21)&0x3FFu)<<1)
             | ((instr & 0x80000000u) ? 0xFFE00000u : 0u);
    if (op == 0x37 || op == 0x17)          // U-type
        return instr & 0xFFFFF000u;
    if (op == 0x23)                        // S-type
        return (uint32_t)(((int32_t)(instr & 0xFE000000) >> 20) | (int32_t)((instr >> 7) & 0x1F));
    return (uint32_t)((int32_t)instr >> 20);   // I-type (don't-care for R-type)
}

template<bool NC1> __global__ void __launch_bounds__(256)
rv32i_kernel(CoreState* st, uint32_t* mem, int ncores, int budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g  = st[id];
    extern __shared__ uint32_t s_regs[];
    uint32_t* const regs = &s_regs[threadIdx.x * 33];
    #pragma unroll
    for (int i = 0; i < 32; i++) regs[i] = g.regs[i];
    uint32_t pc = g.pc;

    for (int n = 0; n < budget && (int32_t)pc >= 0; n++) {
        // Fetch the instruction live from this core's RAM (correct for self-modifying / JIT guests),
        // then assemble its immediate inline.
        const uint32_t instr = ld_i<uint32_t,NC1>(mem, ncores, id, pc);
        const uint32_t d0    = decode_imm(instr);
        const int      rd = (instr >> 7) & 0x1F;
        const uint32_t f3 = (instr >> 12) & 0x7, f7 = (instr >> 25) & 0x7F;
        const uint32_t u1 = regs[(instr >> 15) & 0x1F], u2 = regs[(instr >> 20) & 0x1F];
        const int32_t  s1 = (int32_t)u1, s2 = (int32_t)u2;
        const int      sh = (int)(d0 & 0x1F);            // I-type shamt = iimm[4:0]
        uint32_t nextpc = pc + 4, r = 0;

        const uint32_t op = instr & 0x7F;
        if (op == 0x13) {
            r = alu(f3, u1, d0, (uint32_t)sh, false, f7 == 0x20);
        }
        else if (op == 0x33) {
            if (f7 == 0x01) {                            // M extension (exact RV32M semantics)
                switch (f3) {
                    case 0: r = u1 * u2; break;
                    case 1: r = (uint32_t)(((int64_t)s1 * (int64_t)s2) >> 32); break;
                    case 2: r = (uint32_t)(((int64_t)s1 * (int64_t)(uint64_t)u2) >> 32); break;
                    case 3: r = (uint32_t)(((uint64_t)u1 * (uint64_t)u2) >> 32); break;
                    case 4: r = (u2==0) ? 0xFFFFFFFFu : (s1==(int32_t)0x80000000 && s2==-1) ? 0x80000000u : (uint32_t)(s1/s2); break;
                    case 5: r = (u2==0) ? 0xFFFFFFFFu : (u1/u2); break;
                    case 6: r = (u2==0) ? u1 : (s1==(int32_t)0x80000000 && s2==-1) ? 0u : (uint32_t)(s1%s2); break;
                    default:r = (u2==0) ? u1 : (u1%u2); break;
                }
            }
            else if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) { pc |= HALT_BIT; continue; }
            else r = alu(f3, u1, u2, (uint32_t)(s2 & 0x1F), f7 == 0x20, f7 == 0x20);
        }
        else if (op == 0x03) {
            uint32_t addr = (uint32_t)(s1 + (int32_t)d0);
            switch (f3) {
                case 0: r = (uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem, ncores, id, addr); break;
                case 1: r = (uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem, ncores, id, addr); break;
                case 2: r =                    ld_i<uint32_t,NC1>(mem, ncores, id, addr); break;
                case 4: r =                    ld_i<uint8_t,NC1> (mem, ncores, id, addr); break;
                case 5: r =                    ld_i<uint16_t,NC1>(mem, ncores, id, addr); break;
                default: pc |= HALT_BIT; continue;
            }
        }
        else if (op == 0x63) {
            int taken = 0;
            switch (f3) { case 0: taken = u1==u2; break; case 1: taken = u1!=u2; break;
                          case 4: taken = s1<s2; break;  case 5: taken = s1>=s2; break;
                          case 6: taken = u1<u2; break;  case 7: taken = u1>=u2; break;
                          default: pc |= HALT_BIT; continue; }
            pc = taken ? pc + d0 : nextpc; continue;
        }
        else if (op == 0x23) {
            uint32_t addr = (uint32_t)(s1 + (int32_t)d0);
            switch (f3) { case 0: st_i<uint8_t,NC1>(mem,ncores,id,addr,(uint8_t)u2); break;
                          case 1: st_i<uint16_t,NC1>(mem,ncores,id,addr,(uint16_t)u2); break;
                          case 2: st_i<uint32_t,NC1>(mem,ncores,id,addr,u2); break;
                          default: pc |= HALT_BIT; continue; }
            pc = nextpc; continue;
        }
        else if (op == 0x6F) {
            r = pc + 4;
            nextpc = pc + d0;
        }
        else if (op == 0x67) {
            r = pc + 4; nextpc = (uint32_t)(s1 + (int32_t)d0) & ~1u;
        }
        else if (op == 0x37) {
            r = d0;
        }
        else if (op == 0x17) {
            r = pc + d0;
        }
        else if (op == 0x0F) { pc = nextpc; continue; }
        else { pc |= HALT_BIT; continue; }

        if (rd) regs[rd] = r;
        pc = nextpc;
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = regs[i];
    g.pc = pc;
}

// ===================================================================
//  rvcud — optional RV32I→CUDA-uarch translator (JIT-style pretransform).
//  A host pass rewrites the guest into a denser uop stream (one uop = one
//  or more fused guest instructions) executed by rvcud_kernel. The default
//  rv32i_kernel is unchanged. Win lever (per the measured throughput-bound
//  model): FEWER uops executed + less divergence — never jump tables.
//  uop = uint2{w0,w1}; w0 packs class/regs/f3/flags/predicate; w1 a payload.
//  Sibling arrays (read-only, broadcast): uw[] guest-instr weight per uop,
//  pc2uop[] guest-word→uop index (entry + JALR), uop2pc[] uop→guest pc.
// ===================================================================
// Class codes are SPARSE (opcode-like), NOT a dense 0..N — a dense switch makes ptxas
// emit BRX (an indirect jump), which measured -35%. Sparse values keep the dispatch a
// frequency-ordered predicated compare-ladder, exactly like rv32i_kernel's op-ladder.
//   RC_MULADD: r = root*C + reg (one IMAD). RC_XSH: r = rs ^ (rs<<|>>k) (xorshift step).
//   RC_INCBR: rc += K; if (rc cmp rX) goto T (counted-loop addi+branch → 1 uop).
//   RC_LOADPI/RC_STOREPI: load/store at 0(base) + post-increment base by K (pointer-walk copy loops).
//   RC_LDX/RC_STX: indexed load/store rd=mem[ra+rb+imm] / mem[ra+rb+imm]=rc (add+load|store → 1 uop;
//                  the address temp is a virtual register — never written to the shared regfile).
enum { RC_ALUI=0x13, RC_ALUR=0x33, RC_MULC=0x0B, RC_LOAD=0x03, RC_BR=0x63, RC_ST=0x23,
       RC_CONST=0x37, RC_LEA=0x1B, RC_ADDC=0x2B, RC_JAL=0x6F, RC_JALR=0x67, RC_NOP=0x0F,
       RC_SUB=0x3B, RC_MULR=0x53, RC_MULADD=0x43, RC_XSH=0x4B, RC_INCBR=0x5B,
       RC_LOADPI=0x71, RC_STOREPI=0x79, RC_COPYPI=0x75, RC_LDX=0x6D, RC_STX=0x65, RC_LDXS=0x1D, RC_STXS=0x15,
       RC_COPYLOOP=0x6B, RC_MEXT=0x5D, RC_MULLOOP=0x5F, RC_DIVLOOP=0x49,
       RC_MEMSET=0x39, RC_PALEXP=0x4F, RC_TEXSPAN=0x59, RC_COPYLOOPS=0x21, RC_TEXCOL=0x61,
       RC_WORDFILL=0x35, RC_WORDSCAN=0x45, RC_COPYLOOPT=0x55, RC_ILL=0x00 };
       // RC_COPYLOOPT: strided copy whose src-advance goes through a temp (lX V,0(S); addi S2,S,Ks;
       //   sX V,0(D); addi D,D,Kd; mv S,S2; bne S2,LIM) → 1 native loop uop (params in g_ext).
       //   The COPYLOOPS matcher requires in-place addi src bumps and misses this 6-word form
       //   (profiled hot: 5 dispatched uops/iter as 1:1 code). Generic.
       // RC_WORDFILL: direct-form fill loop (sX VAL,0(D); addi D,D,K; bne D,END) → 1 native loop uop.
       //   The byte-MEMSET arm matches clang's addi-temp form; this matches the temp-less direct form
       //   (profiled hot: 2 dispatched uops/iter as 1:1 code). Generic, any width/stride.
       // RC_WORDSCAN: counted search loop (addi C,C,Kc; addi P,P,Kp; b<cc> C,Z,→EXIT; lw V,off(P);
       //   bne V,KEY,→top) → 1 native loop uop (params in g_ext; a record-table scan, profiled at
       //   ~15% of all uop-execs at 4 dispatched uops/iter). Two exits, EXACT per-path weights (3 vs 5).
       // RC_TEXCOL: textured-column loop: pix=cmap[tex[(frac<<sa)>>sb]]; *dst=pix; dst+=stride;
       //   frac+=step → 1 native loop. 1D vertical TEXSPAN; params (regs/offsets/shifts/stride) in g_ext, generic.
       // RC_COPYLOOPS: strided copy loop (load rt,0(src); store rt,0(dst); src+=Ks; dst+=Kd; bne cnt,lim) → 1
       //   native loop. Generic (any element size + arbitrary strides); captures column/span blits. Params in g_ext.
       // RC_MEMSET: byte-fill loop (libc memset/bzero) → 1 native loop uop.
       // NATIVE_LOOP transform kinds (#13/#20 — generic strided gather→transform→scatter loops; the loop's KIND
       // is recognized structurally and its params (regs/offsets/shifts/strides) are READ from the matched
       // instructions, never hardcoded → generic across guests):
       // RC_TEXSPAN: texture-span loop  pix=cmap[tex[((y>>shY)&mask)+((x<<shX1)>>shX2)]]; *dst++=pix; x+=xs; y+=ys.
       // RC_PALEXP:  LUT-expand loop     idx=src++; p=tbl+3*idx; dst[0..2]=p[0..2]; dst[3]=const; dst+=4.
       // RC_DIVLOOP: clang's rv32i bit-serial restoring-division loop (the body shared by __udivsi3/__divsi3,
       //   ~416 guest instrs) → ONE hardware divide. Q=N/D, R=N%D, counter→-1. CPU-validated 8M cases.
       // COPYLOOP: byte-memcpy loop; MEXT: mulh*/div*/rem*; MULLOOP: whole shift-add software-multiply loop → 1 hw multiply
enum { RP_UNC=0, RP_GP=1, RP_GNP=2, RP_SETP=3 };   // uop predicate: unconditional / guard-P / guard-!P / set-P
static constexpr uint32_t RC_BADUOP = 0x80000000u;   // pc2uop sentinel: not a uop leader → halt on landing

// w0: class[6:0] rd[11:7] rs1[16:12] rs2[21:17] f3[24:22] sra[25] pred[28:26] selc[31:29]
#define RCW0(cls,rd,rs1,rs2,f3,sra,pred,selc) \
    ((uint32_t)(cls) | ((uint32_t)(rd)<<7) | ((uint32_t)(rs1)<<12) | ((uint32_t)(rs2)<<17) \
     | ((uint32_t)(f3)<<22) | ((uint32_t)(sra)<<25) \
     | ((uint32_t)(pred)<<26) | ((uint32_t)(selc)<<29))

__device__ unsigned long long g_dev_iters = 0;   // DEBUG: total uop-loop iterations (core 0) — clock-independent
#if RVCUD_HOTHIST
__device__ unsigned long long* g_dev_hot = nullptr;   // profiling: per-uop execution count (core 0)
#endif
template<bool NC1> __global__ void __launch_bounds__(256)
rvcud_kernel(CoreState* __restrict__ st, uint32_t* __restrict__ mem, const uint2* __restrict__ uops, const uint8_t* __restrict__ uw,
             const uint32_t* __restrict__ pc2uop, const uint32_t* __restrict__ uop2pc,
             const uint32_t* __restrict__ ext,
             int ncores, int nwords, uint32_t base, int budget, int xstop, unsigned long long* __restrict__ retd) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    extern __shared__ uint32_t s_regs[];
    uint32_t* const regs = &s_regs[threadIdx.x * 33];   // 33-stride: bank-conflict-free for 32 banks
    #pragma unroll
    for (int i = 0; i < 32; i++) regs[i] = g.regs[i];
    uint32_t pc = g.pc;
    // entry: map guest pc → uop index (halt if out of range or a non-leader landing)
    uint32_t ui;
    if ((int32_t)pc < 0)                  ui = HALT_BIT;
    else { uint32_t pw = (pc - base) >> 2;
           ui = (pw < (uint32_t)nwords) ? __ldg(&pc2uop[pw]) : HALT_BIT;
           if (ui & 0x80000000u) ui = HALT_BIT; }
    uint32_t P = 0;                      // if-conversion predicate
    uint32_t resume_pc = HALT_BIT;       // set to a guest pc if we stop at an untranslated JALR target

    int gi = 0; unsigned long long iters = 0;
    for (; gi < budget && (int32_t)ui >= 0; ) {
        iters++;
#if RVCUD_HOTHIST
        if (id == 0 && g_dev_hot) g_dev_hot[ui]++;          // profiling only (perturbs MIPS; off by default)
#endif
        const uint32_t w0 = __ldg(&uops[ui].x);     // two 32-bit loads, one cache line (never LDG.E.64)
        const uint32_t w1 = __ldg(&uops[ui].y);
        const uint32_t cls  = w0 & 0x7F;
        const int      rd   = (w0 >> 7) & 0x1F;
        const uint32_t u1   = regs[(w0 >> 12) & 0x1F];
        const uint32_t u2   = regs[(w0 >> 17) & 0x1F];
        const uint32_t f3   = (w0 >> 22) & 7;
        const bool     sra  = (w0 >> 25) & 1;
        const uint32_t pred = (w0 >> 26) & 7;
        const uint32_t selc = (w0 >> 29) & 7;
        const int32_t  s1 = (int32_t)u1, s2 = (int32_t)u2;
        const int      uwb = __ldg(&uw[ui]);         // bit7 = exec-entry stop flag; bits 6:0 = retired weight
        const int      wt  = uwb & 0x7F;             // guest-instructions this uop retires
        // exec-entry self-stop: when the exec_block is live (xstop==0x80) and this uop's pc is a
        // dispatch entry, exit BEFORE executing it (gi>0: always make progress first) — the driver
        // re-enters the register-resident exec_block there instead of interpreting onward. This is
        // what lets the dispatch-entry set be SPARSE without stranding whole chunks in the interpreter.
        if ((uwb & xstop) && gi) break;

        // predicate-producing uops: compute P / SEL, no rd write, fall through linearly
        if (pred == RP_SETP) {
            int t; switch (f3) { case 0:t=u1==u2;break; case 1:t=u1!=u2;break; case 4:t=s1<s2;break;
                                 case 5:t=s1>=s2;break; case 6:t=u1<u2;break; default:t=u1>=u2; }
            P = (selc & 1) ? (uint32_t)(!t) : (uint32_t)t;   // selc bit0 = invert (body-live polarity)
            ui += 1; gi += wt; continue;
        }
        uint32_t live = 1;
        if (pred) live = (pred==RP_GP) ? P : (P ^ 1u);   // pred ∈ {GP,GNP} for if-converted body uops

        // Frequency-ordered dispatch ladder (hottest classes first → fewest predicted compares on
        // the common path). SUB/MULR are rare (never fire on these guests) → pushed to the tail.
        uint32_t r = 0, nui = ui + 1;
        // Frequency-ordered by the dynamic profile of the target guest workload: hottest classes first so the common
        // path hits the fewest predicted compares. At ~48 cyc/uop each ladder compare is ~3%, so order
        // matters a lot. Universally-hot (ALUI/ALUR/LOAD/ST/BR) lead; profiled-hot fusions next; compute-only
        // (XSH/ADDC/MULC) and dormant-on-rv32i (MULR/MEXT) at the tail. Reorder is semantically identical.
        if      (cls == RC_ALUI)  r = alu(f3, u1, w1, w1 & 0x1F, false, sra);
        else if (cls == RC_BR) {                         // 2nd: 17.6% of profiled execs (was 5th)
            int t; switch (f3) { case 0:t=u1==u2;break; case 1:t=u1!=u2;break; case 4:t=s1<s2;break;
                                 case 5:t=s1>=s2;break; case 6:t=u1<u2;break; default:t=u1>=u2; }
            ui = t ? w1 : ui + 1; gi += wt; continue;                            // w1 = baked target uop-index
        }
        else if (cls == RC_ALUR)  r = alu(f3, u1, u2, (uint32_t)(s2 & 0x1F), false, sra);
        else if (cls == RC_LOAD) { uint32_t a = (uint32_t)(s1 + (int32_t)w1);
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t,NC1>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t,NC1>(mem,ncores,id,a); } }   // f3==5 (translator validated {0,1,2,4,5})
        else if (cls == RC_SUB)   r = u1 - u2;           // 5th: 5.7% of profiled execs (was tail)
        else if (cls == RC_ST) { uint32_t a = (uint32_t)(s1 + (int32_t)w1);
            if (live) switch (f3) { case 0: st_i<uint8_t,NC1> (mem,ncores,id,a,(uint8_t)u2);  break;
                                    case 1: st_i<uint16_t,NC1>(mem,ncores,id,a,(uint16_t)u2); break;
                                    default:st_i<uint32_t,NC1>(mem,ncores,id,a,u2); }           // @p st.global when predicated
            ui += 1; gi += live ? wt : 0; continue; }    // count only if the guest would have run it
        else if (cls == RC_INCBR) {                                             // counted loop: rc += K; if (rc cmp rX) goto T
            int32_t inc = (int32_t)w1 >> 24;                                     // signed high byte = increment
            uint32_t tgt = w1 & 0x00FFFFFFu;                                     // low 24 = baked target uop-index (0xFFFFFF = unresolved → halt)
            uint32_t nv = u1 + (uint32_t)inc; int32_t sv = (int32_t)nv;
            if (rd) regs[rd] = nv;                                              // write the (incremented) counter
            int t; switch (f3) { case 0:t=nv==u2;break; case 1:t=nv!=u2;break; case 4:t=sv<s2;break;
                                 case 5:t=sv>=s2;break; case 6:t=nv<u2;break; default:t=nv>=u2; }
            ui = t ? (tgt==0x00FFFFFFu ? HALT_BIT : tgt) : ui + 1; gi += wt; continue;
        }
        else if (cls == RC_LOADPI) {                     // r = load[base]; base += w1  (load + post-increment)
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,u1); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,u1); break;
                          case 2: r=                   ld_i<uint32_t,NC1>(mem,ncores,id,u1); break;
                          case 4: r=                   ld_i<uint8_t,NC1> (mem,ncores,id,u1); break;
                          default:r=                   ld_i<uint16_t,NC1>(mem,ncores,id,u1); }
            regs[(w0>>12)&0x1F] = u1 + w1;               // rd != base (translator-enforced) → both writes safe
            pf_i(mem, ncores, id, u1 + w1); }            // prefetch next iteration's source (this loop's stride)
        else if (cls == RC_STOREPI) {                    // store[base] = rs2; base += w1  (store + post-increment)
            switch (f3) { case 0: st_i<uint8_t,NC1> (mem,ncores,id,u1,(uint8_t)u2);  break;
                          case 1: st_i<uint16_t,NC1>(mem,ncores,id,u1,(uint16_t)u2); break;
                          default:st_i<uint32_t,NC1>(mem,ncores,id,u1,u2); }
            regs[(w0>>12)&0x1F] = u1 + w1; ui += 1; gi += wt; continue; }
        else if (cls == RC_LDX) { uint32_t a = u1 + u2 + w1;          // rd = mem[ra+rb+imm]  (add + load → 1 uop)
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t,NC1>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t,NC1>(mem,ncores,id,a); } }   // falls through to rd writeback
        else if (cls == RC_LDXS) {                                    // rd = mem[(ra<<k)+rb+imm]  (slli+add+load → 1 uop)
            uint32_t k = (w1>>24)&0x1F; int32_t imm = ((int32_t)(w1 & 0x00FFFFFF) << 8) >> 8;   // scale k; sign-ext 24→32
            uint32_t a = (u1 << k) + u2 + (uint32_t)imm;              // scaled address lives only in this CUDA register
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t,NC1>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t,NC1> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t,NC1>(mem,ncores,id,a); } }   // falls through to rd writeback
        else if (cls == RC_STXS) {                                    // mem[(ra<<k)+rb+imm] = rc  (slli+add+store → 1 uop)
            uint32_t k = (w1>>24)&0x1F; int32_t imm = ((int32_t)(w1 & 0x00FFFFFF) << 8) >> 8;
            uint32_t a = (u1 << k) + u2 + (uint32_t)imm; uint32_t val = regs[(w0>>7)&0x1F];   // rc carried in rd field
            switch (f3) { case 0: st_i<uint8_t,NC1> (mem,ncores,id,a,(uint8_t)val);  break;
                          case 1: st_i<uint16_t,NC1>(mem,ncores,id,a,(uint16_t)val); break;
                          default:st_i<uint32_t,NC1>(mem,ncores,id,a,val); }
            ui += 1; gi += wt; continue; }
        else if (cls == RC_MULADD) r = u1 * w1 + u2;                             // affine ×const tree + live const-reg → 1 IMAD
        else if (cls == RC_COPYPI) {                     // mem[dst]=mem[src]; rt=loaded; src+=Ks; dst+=Kd  (lb;sb;addi;addi → 1 uop)
            uint32_t t;                                  // u1=src base, u2=dst base; w1 = Ks(lo16) | Kd(hi16); rt = rd field
            switch (f3) {                                // t = sign/zero-extended load (mirrors lb/lbu/lh/lhu/lw); store uses low bits
                case 0: t=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,u1); st_i<uint8_t,NC1> (mem,ncores,id,u2,(uint8_t)t);  break;
                case 1: t=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,u1); st_i<uint16_t,NC1>(mem,ncores,id,u2,(uint16_t)t); break;
                case 2: t=                   ld_i<uint32_t,NC1>(mem,ncores,id,u1); st_i<uint32_t,NC1>(mem,ncores,id,u2,t);           break;
                case 4: t=                   ld_i<uint8_t,NC1> (mem,ncores,id,u1); st_i<uint8_t,NC1> (mem,ncores,id,u2,(uint8_t)t);  break;
                default:t=                   ld_i<uint16_t,NC1>(mem,ncores,id,u1); st_i<uint16_t,NC1>(mem,ncores,id,u2,(uint16_t)t); }
            if (rd) regs[rd] = t;                                                  // rt = loaded value (rt != src,dst)
            regs[(w0>>12)&0x1F] = u1 + (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFF);   // src += Ks
            regs[(w0>>17)&0x1F] = u2 + (uint32_t)(int32_t)(int16_t)(w1 >> 16);      // dst += Kd
            pf_i(mem, ncores, id, u1 + (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFF));  // prefetch next iteration's source
            ui += 1; gi += wt; continue; }
        else if (cls == RC_MULLOOP) {                    // whole shift-add software-multiply loop → 1 hardware multiply
            { uint32_t ftp = w1 & 0x00FFFFFFu;           // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x00FFFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            uint32_t M0 = u1, B0 = u2, ACC0 = regs[rd];  // rs1=multiplier(→0), rs2=multiplicand(<<iters), rd=accumulator
            unsigned it = M0 ? (32u - (unsigned)__clz(M0)) : 1u;                    // exact loop iteration count (do-while ≥1)
            regs[rd]            = ACC0 + B0 * M0;                                   // ACC += B*M  (hardware multiply)
            regs[(w0>>12)&0x1F] = 0u;                                              // M shifted to 0
            regs[(w0>>17)&0x1F] = (it >= 32) ? 0u : (B0 << it);                    // B shifted left once per iteration
            regs[(w1>>24)&0x1F] = (M0==0) ? 0u : ((it-1 >= 32) ? 0u : (B0 << (it-1)));  // A = last-iteration mask & B
            gi += 7u * it;                                                         // EXACT RV32I instructions the loop ran
            uint32_t ft = w1 & 0x00FFFFFFu;
            if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 28; break; } // 7 words; unresolved → translate-on-miss
            ui = ft; continue; }
        else if (cls == RC_DIVLOOP) {                    // bit-serial restoring-division loop → one hardware divide
            { uint32_t ftp = w1 & 0x003FFFFFu;           // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x003FFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            uint32_t Rr=(w0>>12)&0x1F, Nr=(w0>>17)&0x1F; // rd=Q (quotient); rs1=R (remainder); rs2=N (dividend)
            uint32_t Dr=(w1>>22)&0x1F, ir=(w1>>27)&0x1F; // D=divisor; i=down-counter (→ -1). Matcher verified the
            uint32_t Nv=regs[Nr], Dv=regs[Dr];           // prologue sets R=0,Q=0,i=31,ONE=1,NEG1=-1 (32-bit udiv).
            regs[rd] = Dv ? (Nv / Dv) : 0xFFFFFFFFu;     // Q = N / D
            regs[Rr] = Dv ? (Nv % Dv) : Nv;              // R = N % D
            regs[ir] = 0xFFFFFFFFu;                      // counter ends at -1
            gi += 13u * 32u;                             // 13 instrs × 32 iterations (EXACT retired count)
            uint32_t ft = w1 & 0x003FFFFFu;              // low 22 bits = fall-through uop index (baked Pass 3)
            if (ft == 0x003FFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 13*4; break; }  // unresolved → translate-on-miss
            ui = ft; continue; }
        else if (cls == RC_MEMSET) {                     // byte-fill loop (memset) → one native loop
            uint32_t D = regs[rd], VAL = u1, END = u2, nDr = (w1>>24)&0x1F;   // rd=DST, rs1=VAL, rs2=END (VAL,END invariant)
            { uint32_t ftp = w1 & 0x00FFFFFFu;           // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x00FFFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            while (D != END && gi < budget) { st_i<uint8_t,NC1>(mem,ncores,id,D,(uint8_t)VAL); D += 1; gi += 4; }  // 4 instrs/iter
            regs[rd] = D; regs[nDr] = D;                 // DST→END; nD (temp) = END
            if (D == END) { uint32_t ft = w1 & 0x00FFFFFFu;
                if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 16; break; }   // 4 words; unresolved → translate-on-miss
                ui = ft; }
            continue; }
        else if (cls == RC_WORDFILL) {                   // direct-form fill loop: sX VAL,0(D); D+=K; bne D,END,→top
            uint32_t D = regs[rd], VAL = u1, END = u2; int32_t K = (int32_t)w1 >> 24;   // rd=D, rs1=VAL (may be x0), rs2=END
            { uint32_t ftp = w1 & 0x00FFFFFFu;           // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x00FFFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            do {                                         // guest stores BEFORE testing the bne (do-while shape)
                switch (f3) { case 0: st_i<uint8_t,NC1> (mem,ncores,id,D,(uint8_t)VAL);  break;
                              case 1: st_i<uint16_t,NC1>(mem,ncores,id,D,(uint16_t)VAL); break;
                              default:st_i<uint32_t,NC1>(mem,ncores,id,D,VAL); }
                D += (uint32_t)K; gi += 3;               // store + addi + bne
            } while (D != END && gi < budget);
            regs[rd] = D;
            if (D == END) { uint32_t ft = w1 & 0x00FFFFFFu;
                if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 12; break; }   // 3 words; unresolved → translate-on-miss
                ui = ft; }
            continue; }                                  // else budget-cut → ui stays, resume re-enters the loop
        else if (cls == RC_WORDSCAN) {                   // counted search loop: whole loop, native (params in ext[])
            uint32_t e = w0 >> 7;                                              // ext base index
            uint32_t p0=__ldg(&ext[e]), p1=__ldg(&ext[e+1]), xt=__ldg(&ext[e+2]);
            uint32_t Cr=p0&31, Pr=(p0>>5)&31, Vr=(p0>>10)&31, Kr=(p0>>15)&31, Zr=(p0>>20)&31, cf3=(p0>>25)&7;
            int32_t Kc=(int32_t)(int8_t)(p1&0xFF), Kp=(int32_t)(int8_t)((p1>>8)&0xFF);
            int32_t off=((int32_t)p1)>>20;                                     // signed 12-bit load offset (hi 12 of p1)
            uint32_t C=regs[Cr], P=regs[Pr], V=regs[Vr], KEY=regs[Kr], Z=regs[Zr];
            { if (w1 != 0xFFFFFFFFu) { pf_h(&uops[w1]); pf_h(&uw[w1]); } }     // warm fall-through dispatch (hint)
            uint32_t exited = 0;                                               // 1 = C-branch taken (forward exit)
            do {                                                               // guest order: addi C; addi P; b<cc>; lw; bne
                C += (uint32_t)Kc; P += (uint32_t)Kp;
                int32_t sc=(int32_t)C, sz=(int32_t)Z; int t;
                switch (cf3) { case 0:t=C==Z;break; case 1:t=C!=Z;break; case 4:t=sc<sz;break;
                               case 5:t=sc>=sz;break; case 6:t=C<Z;break; default:t=C>=Z; }
                if (t) { gi += 3; exited = 1; break; }                         // exit branch taken: 3 retired this iter
                V = ld_i<uint32_t,NC1>(mem,ncores,id, P + (uint32_t)off);
                gi += 5;                                                       // full iteration: 5 retired
            } while (V != KEY && gi < budget);
            regs[Cr]=C; regs[Pr]=P; regs[Vr]=V;                                // KEY/Z read-only
            if (exited) {                                                      // → baked forward-exit uop
                if (xt == 0xFFFFFFFFu) { ui |= HALT_BIT; continue; }           // unresolved exit target → halt (translator bakes it)
                ui = xt; continue; }
            if (V == KEY) {                                                    // match → fall through past the bne
                if (w1 == 0xFFFFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 5*4; break; }
                ui = w1; continue; }
            continue; }                                                        // budget-cut → ui stays, resume re-enters
        else if (cls == RC_COPYLOOPT) {                    // temp-advance strided copy: whole loop, native (params in ext[])
            uint32_t e = w0 >> 7; uint32_t p0=__ldg(&ext[e]), p1=__ldg(&ext[e+1]);
            uint32_t Sr=p0&31, Dr=(p0>>5)&31, Vr=(p0>>10)&31, S2r=(p0>>15)&31, Lr=(p0>>20)&31, lf3=(p0>>25)&7;
            int32_t Ks=(int32_t)(int16_t)(p1&0xFFFF), Kd=(int32_t)(int16_t)(p1>>16);
            uint32_t S=regs[Sr], D=regs[Dr], LIM=regs[Lr], v=0, S2v;
            { if (w1 != 0xFFFFFFFFu) { pf_h(&uops[w1]); pf_h(&uw[w1]); } }     // warm exit dispatch (hint)
            do {                                                               // guest order: load; S2=S+Ks; store; D+=Kd; S=S2; bne
                switch (lf3) { case 0: v=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,S); st_i<uint8_t,NC1> (mem,ncores,id,D,(uint8_t)v);  break;
                               case 1: v=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,S); st_i<uint16_t,NC1>(mem,ncores,id,D,(uint16_t)v); break;
                               case 2: v=                   ld_i<uint32_t,NC1>(mem,ncores,id,S); st_i<uint32_t,NC1>(mem,ncores,id,D,v);           break;
                               case 4: v=                   ld_i<uint8_t,NC1> (mem,ncores,id,S); st_i<uint8_t,NC1> (mem,ncores,id,D,(uint8_t)v);  break;
                               default:v=                   ld_i<uint16_t,NC1>(mem,ncores,id,S); st_i<uint16_t,NC1>(mem,ncores,id,D,(uint16_t)v); }
                S2v = S + (uint32_t)Ks; D += (uint32_t)Kd; S = S2v; gi += 6;   // 6 guest instrs/iteration
            } while (S2v != LIM && gi < budget);
            regs[Sr]=S; regs[Dr]=D; regs[Vr]=v; regs[S2r]=S2v;                 // LIM read-only
            if (S2v == LIM) { if (w1 == 0xFFFFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 6*4; break; }
                ui = w1; }
            continue; }                                                        // else budget-cut → ui stays, resume re-enters
        else if (cls == RC_STX) { uint32_t a = u1 + u2 + w1;          // mem[ra+rb+imm] = rc  (add + store → 1 uop)
            uint32_t val = regs[(w0>>7)&0x1F];                        // rc carried in the rd field
            switch (f3) { case 0: st_i<uint8_t,NC1> (mem,ncores,id,a,(uint8_t)val);  break;
                          case 1: st_i<uint16_t,NC1>(mem,ncores,id,a,(uint16_t)val); break;
                          default:st_i<uint32_t,NC1>(mem,ncores,id,a,val); }
            ui += 1; gi += wt; continue; }
        else if (cls == RC_COPYLOOP) {                   // whole forward unit-stride byte memcpy loop, word-widened
            uint32_t s = u1, d = u2, L = regs[(w1>>24)&0x1F];  // src, dst, limit; counter = src or dst
            const bool cdst = (w0>>25)&1;                      // which pointer the bne compares to L
            { uint32_t ftp = w1 & 0x00FFFFFFu;                 // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x00FFFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            while ((cdst ? d : s) != L && gi < budget) {       // word-widened scalar copy
                uint32_t cnt = cdst ? d : s, rem = L - cnt, adiff = (d > s) ? (d - s) : (s - d);
                pf_i(mem, ncores, id, s + 96);                 // prefetch source ~96B ahead of the copy cursor
                if (rem >= 4u && adiff >= 4u) {                // word copy: ≥4 to go AND non-overlapping within the word
                    st_i<uint32_t,NC1>(mem,ncores,id,d, ld_i<uint32_t,NC1>(mem,ncores,id,s));
                    s += 4; d += 4; gi += 20;                  // 4 byte-iterations (×5 guest instrs)
                } else {                                       // byte copy (tail / overlap)
                    st_i<uint8_t,NC1>(mem,ncores,id,d,(uint8_t)ld_i<uint8_t,NC1>(mem,ncores,id,s));
                    s += 1; d += 1; gi += 5;
                }
            }
            uint32_t lastb = ld_i<uint8_t,NC1>(mem,ncores,id,s-1);
            if (rd) regs[rd] = (f3==0) ? (uint32_t)(int8_t)lastb : lastb;          // rt = last loaded byte (may be live)
            regs[(w0>>12)&0x1F] = s; regs[(w0>>17)&0x1F] = d;                       // src, dst
            if ((cdst ? d : s) == L) {                                             // loop finished → fall through
                uint32_t ft = w1 & 0x00FFFFFFu;
                if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 20; break; }   // unresolved → translate-on-miss
                ui = ft;
            }                                                                      // else budget-cut → ui stays, resume here
            continue; }
        else if (cls == RC_XSH)   r = u1 ^ (sra ? (u1 >> (w1 & 31)) : (u1 << (w1 & 31)));  // xorshift step: rs ^ (rs<<|>>k)
        else if (cls == RC_ADDC)  r = u1 + w1;
        else if (cls == RC_MULC)  r = u1 * w1;                                   // strength-reduced ×const → 1 IMAD
        else if (cls == RC_LEA)   r = (u1 << (w1 & 31)) + u2;                    // slli+add fused
        else if (cls == RC_CONST) r = w1;
        else if (cls == RC_MULR)  r = u1 * u2;                                   // M-ext MUL (low 32)
        else if (cls == RC_MEXT) {                                               // M-ext high-mul / div / rem (exact RV semantics)
            switch (f3) {
                case 1: r = (uint32_t)(((int64_t)s1 * (int64_t)s2) >> 32); break;                          // mulh
                case 2: r = (uint32_t)(((int64_t)s1 * (int64_t)(uint64_t)u2) >> 32); break;                // mulhsu
                case 3: r = (uint32_t)(((uint64_t)u1 * (uint64_t)u2) >> 32); break;                        // mulhu
                case 4: r = (u2==0) ? 0xFFFFFFFFu : (s1==(int32_t)0x80000000 && s2==-1) ? 0x80000000u : (uint32_t)(s1/s2); break;  // div
                case 5: r = (u2==0) ? 0xFFFFFFFFu : (u1/u2); break;                                        // divu
                case 6: r = (u2==0) ? u1 : (s1==(int32_t)0x80000000 && s2==-1) ? 0u : (uint32_t)(s1%s2); break;  // rem
                default:r = (u2==0) ? u1 : (u1%u2); }                                                      // remu
        }
        else if (cls == RC_JAL)  { if (rd && live) regs[rd] = __ldg(&uop2pc[ui]) + 4; ui = w1; gi += wt; continue; }  // skip the link load for `j` (rd==0)
        else if (cls == RC_JALR) { uint32_t tgt = (uint32_t)(s1 + (int32_t)w1) & ~1u;
            uint32_t tw = (tgt - base) >> 2;
            uint32_t n2 = (tw < (uint32_t)nwords) ? __ldg(&pc2uop[tw]) : RC_BADUOP;
            if (rd && live) regs[rd] = __ldg(&uop2pc[ui]) + 4;   // skip link load for indirect jump `jalr x0` (rd==0)
            gi += wt;
            // Indirect target with no uop: stop AT tgt so the host can translate that block on
            // demand (translate-on-miss), then resume. tgt itself is a valid in-range code address.
            if (n2 & 0x80000000u) { resume_pc = (tw < (uint32_t)nwords) ? tgt : HALT_BIT; break; }
            ui = n2; continue; }
        else if (cls == RC_PALEXP) {                     // 8bpp→32bpp palette expand: whole loop, native
            uint32_t SRC=regs[(w0>>7)&0x1F], DST=regs[(w0>>12)&0x1F], PAL=regs[(w0>>17)&0x1F];
            uint32_t END=regs[(w1>>22)&0x1F], A=regs[(w1>>27)&0x1F];   // END, ALPHA invariant; PAL invariant
            { uint32_t ftp = w1 & 0x003FFFFFu;                         // warm the exit dispatch's descriptor loads (hint only)
              if (ftp != 0x003FFFFFu) { pf_h(&uops[ftp]); pf_h(&uw[ftp]); } }
            // LUT fast path: long run → pre-expand the 256×3 B palette to RGBA32 once, then the inner
            // loop has zero per-pixel address ALU. Legal only if the loop's own stores can't touch the
            // palette (the guest re-reads pal[] every pixel, so a store into it must stay observable).
            uint32_t rem = END - SRC;                                  // pixels left this entry
            uint32_t st_lo = DST - 3u, st_hi = st_lo + 4u*rem, pal_hi = PAL + 768u;
            if (rem >= 512u && rem < 0x20000000u && gi < budget        // bound: 4*rem can't alias-wrap
                    && st_hi > st_lo && pal_hi > PAL                   // neither range wraps 2^32
                    && (st_hi <= PAL || pal_hi <= st_lo)) {            // store range ∩ palette = ∅
                uint32_t lut[256];                                     // local (L1-backed); dynamic index
                for (int i = 0; i < 256; ++i)
                    lut[i] = (ld_i<uint32_t,NC1>(mem,ncores,id, PAL + 3u*(uint32_t)i) & 0x00FFFFFFu) | ((A & 0xFFu) << 24);
                while (SRC != END && gi < budget) {                    // identical stores/order/weights
                    st_i<uint32_t,NC1>(mem,ncores,id, DST-3, lut[ld_i<uint8_t,NC1>(mem,ncores,id,SRC)]);
                    SRC += 1; DST += 4; gi += 14;
                }
            }
            else while (SRC != END && gi < budget) {                  // COALESCED: 1 word-load + 1 word-store (was 3+4 byte ops)
                uint32_t p = PAL + 3u * ld_i<uint8_t,NC1>(mem,ncores,id,SRC);
                uint32_t rgb = ld_i<uint32_t,NC1>(mem,ncores,id,p);                                 // R|G<<8|B<<16 (+1 byte masked off)
                st_i<uint32_t,NC1>(mem,ncores,id,DST-3,(rgb & 0x00FFFFFFu) | ((A & 0xFFu) << 24));   // one RGBA word store (== the 4 byte stores)
                SRC += 1; DST += 4; gi += 14;                          // 14 guest instrs/iteration
            }
            regs[(w0>>7)&0x1F] = SRC; regs[(w0>>12)&0x1F] = DST;       // SRC→END, DST advanced
            if (SRC == END) {                                          // loop done → fall through
                uint32_t ft = w1 & 0x003FFFFFu;
                if (ft == 0x003FFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 14*4; break; }
                ui = ft;
            }                                                          // else budget-cut → ui stays, resume here
            continue; }
        else if (cls == RC_TEXSPAN) {                    // texture-mapped span: whole loop, native (params in ext[])
            if (w1 != 0xFFFFFFFFu) { pf_h(&uops[w1]); pf_h(&uw[w1]); }         // warm the exit dispatch's descriptor loads (hint only)
            uint32_t e = w0 >> 7;                                              // ext base index
            uint32_t p0=__ldg(&ext[e]), p1=__ldg(&ext[e+1]), p2=__ldg(&ext[e+2]), p3=__ldg(&ext[e+3]);
            uint32_t Xr=p0&31, Yr=(p0>>5)&31, Dr=(p0>>10)&31, Er=(p0>>15)&31, Mr=(p0>>20)&31, Br=(p0>>25)&31;
            uint32_t oC=p1&0xFFF, oT=(p1>>12)&0xFFF, oX=p2&0xFFF, oY=(p2>>12)&0xFFF;
            uint32_t shY=p3&31, shX1=(p3>>5)&31, shX2=(p3>>10)&31;
            uint32_t X=regs[Xr], Y=regs[Yr], D=regs[Dr], END=regs[Er], MASK=regs[Mr], BASE=regs[Br];
            uint32_t CMAP=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oC), TEX=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oT);  // HOISTED: loop-invariant
            uint32_t XS=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oX), YS=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oY);     // (ds_* globals, disjoint from FB)
            uint32_t acc = 0, npend = 0;                                      // packed FB bytes pending in lanes 0..npend-1
            while (D != END && gi < budget) {                                 // 2 mem-ops/pixel + 1 word-store per 4 (was 7)
                uint32_t off  = ((Y>>shY)&MASK) + ((X<<shX1)>>shX2);
                uint32_t pidx = ld_i<uint8_t,NC1>(mem,ncores,id, TEX + off);
                uint32_t pix  = ld_i<uint8_t,NC1>(mem,ncores,id, CMAP + pidx);
                uint32_t lane = D & 3u;                                       // D advances by +1 → consecutive byte addrs
                if (lane == 0u || npend) {                                    // aligned word in progress: pack, store once full
                    acc |= pix << (lane << 3); npend += 1;                    // LE byte lane == word bits 8*lane..+7
                    if (lane == 3u) { st_i<uint32_t,NC1>(mem,ncores,id, D & ~3u, acc); acc = 0; npend = 0; }
                } else st_i<uint8_t,NC1>(mem,ncores,id, D, (uint8_t)pix);     // unaligned head: byte store as before
                X += XS; Y += YS; D += 1; gi += 19;                           // 19 guest instrs/iteration
            }
            for (uint32_t k = 0; k < npend; k++)                              // tail / budget-cut: flush partial word as
                st_i<uint8_t,NC1>(mem,ncores,id, D - npend + k, (uint8_t)(acc >> (k << 3)));  // byte stores → memory as-if per-pixel
            regs[Xr]=X; regs[Yr]=Y; regs[Dr]=D;                               // XPOS,YPOS final; DST→END
            if (D == END) { uint32_t ft = w1;
                if (ft == 0xFFFFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 19*4; break; }
                ui = ft; }
            continue; }
        else if (cls == RC_COPYLOOPS) {                  // strided copy loop: whole loop, native (params in ext[])
            if (w1 != 0xFFFFFFFFu) { pf_h(&uops[w1]); pf_h(&uw[w1]); }   // warm the exit dispatch's descriptor loads (hint only)
            uint32_t e = w0 >> 7; uint32_t p0=__ldg(&ext[e]), p1=__ldg(&ext[e+1]);
            uint32_t sR=p0&31, dR=(p0>>5)&31, rtR=(p0>>10)&31, lR=(p0>>15)&31, cdst=(p0>>20)&1, f3=(p0>>21)&7;
            int32_t Ks=(int32_t)(int16_t)(p1&0xFFFF), Kd=(int32_t)(int16_t)(p1>>16);
            uint32_t s=regs[sR], d=regs[dR], L=regs[lR], last=0;
            while ((cdst?d:s) != L && gi < budget) {     // mirrors load;store;src+=Ks;dst+=Kd;bne in guest order
                switch (f3) { case 0: last=(uint32_t)(int8_t) ld_i<uint8_t,NC1> (mem,ncores,id,s); st_i<uint8_t,NC1> (mem,ncores,id,d,(uint8_t)last);  break;
                              case 1: last=(uint32_t)(int16_t)ld_i<uint16_t,NC1>(mem,ncores,id,s); st_i<uint16_t,NC1>(mem,ncores,id,d,(uint16_t)last); break;
                              case 2: last=                   ld_i<uint32_t,NC1>(mem,ncores,id,s); st_i<uint32_t,NC1>(mem,ncores,id,d,last);           break;
                              case 4: last=                   ld_i<uint8_t,NC1> (mem,ncores,id,s); st_i<uint8_t,NC1> (mem,ncores,id,d,(uint8_t)last);  break;
                              default:last=                   ld_i<uint16_t,NC1>(mem,ncores,id,s); st_i<uint16_t,NC1>(mem,ncores,id,d,(uint16_t)last); }
                s += (uint32_t)Ks; d += (uint32_t)Kd; gi += 5;       // load,addi,store,addi,bne
            }
            regs[sR]=s; regs[dR]=d; if (rtR) regs[rtR]=last;          // src,dst advanced; rt = last loaded (sign/zero-ext)
            if ((cdst?d:s) == L) { uint32_t ft = w1;
                if (ft == 0xFFFFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 5*4; break; }
                ui = ft; }
            continue; }
        else if (cls == RC_TEXCOL) {                     // textured-column loop: whole loop, native (params in ext[])
            if (w1 != 0xFFFFFFFFu) { pf_h(&uops[w1]); pf_h(&uw[w1]); }   // warm the exit dispatch's descriptor loads (hint only)
            uint32_t e = w0 >> 7; uint32_t p0=__ldg(&ext[e]), p1=__ldg(&ext[e+1]), p2=__ldg(&ext[e+2]);
            uint32_t Fr=p0&31, Dr=(p0>>5)&31, Er=(p0>>10)&31, Br=(p0>>15)&31, Sr=(p0>>20)&31;   // FRAC,DST,END,BASE,STEP
            uint32_t oT=p1&0xFFF, oC=(p1>>12)&0xFFF;
            uint32_t sa=p2&31, sb=(p2>>5)&31; int32_t STR=(int32_t)(int16_t)((p2>>10)&0xFFFF);
            uint32_t F=regs[Fr], D=regs[Dr], END=regs[Er], BASE=regs[Br], STEP=regs[Sr];
            uint32_t TEX=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oT), CMAP=ld_i<uint32_t,NC1>(mem,ncores,id,BASE+oC);  // HOISTED: loop-invariant
            while (D != END && gi < budget) {                                  // 3 mem-ops/pixel (was 5)
                uint32_t t    = (F << sa) >> sb;
                uint32_t pidx = ld_i<uint8_t,NC1>(mem,ncores,id, TEX + t);
                uint32_t pix  = ld_i<uint8_t,NC1>(mem,ncores,id, CMAP + pidx);
                st_i<uint8_t,NC1>(mem,ncores,id, D, (uint8_t)pix);
                D += (uint32_t)STR; F += STEP; gi += 12;                       // 12 guest instrs/iteration
            }
            regs[Dr]=D; regs[Fr]=F;                                            // DST→END; FRAC advanced
            if (D == END) { uint32_t ft = w1;
                if (ft == 0xFFFFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 12*4; break; }
                ui = ft; }
            continue; }
        else if (cls == RC_NOP)  { ui += 1; gi += wt; continue; }
        else { ui |= HALT_BIT; continue; }                                       // RC_ILL

        if (rd && live) regs[rd] = r;                // predicated store (@p st.shared) — no regs[rd] read-back
        ui = nui; gi += live ? wt : 0;               // a predicated-off body uop retires 0 guest instrs
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = regs[i];
    // Resume pc: a missed JALR target (→ host translates it), else the current uop's pc. On halt,
    // PRESERVE the faulting pc next to the bit (the base kernel does; HALT_BIT alone made every
    // guest fault read as pc=0 — undebuggable).
    g.pc = (resume_pc != HALT_BIT) ? resume_pc
         : (((int32_t)ui >= 0)     ? __ldg(&uop2pc[ui])
         : (ui != HALT_BIT)        ? (__ldg(&uop2pc[ui & 0x7FFFFFFFu]) | HALT_BIT)
                                   : (pc | HALT_BIT));
    if (id == 0 && retd) { *retd = (unsigned long long)gi; g_dev_iters += iters; }          // retired guest-instructions (for the verify gate)
}

#define API extern "C" __declspec(dllexport)

static CoreState* g_state  = nullptr;
static uint32_t*  g_mem    = nullptr;   // word-interleaved RAM: [word*ncores + core]
static int        g_ncores = 0;

// rvcud device buffers (built by the host translator in cuda_rvcud_set_code)
static uint2*     g_uops    = nullptr;  // {w0,w1} per uop
static uint8_t*   g_uw      = nullptr;  // guest-instruction weight per uop
static uint32_t*  g_pc2uop  = nullptr;  // guest-word → uop index (or RC_BADUOP)
static uint32_t*  g_uop2pc  = nullptr;  // uop index → guest pc
static uint32_t*  g_ext     = nullptr;  // side params for fat fusions (TEXSPAN); indexed by uop's ext base
static int        g_nuops   = 0;
static int        g_pc2words = 0;       // length of pc2uop[] = translated code words (NOT total memory words)
static uint32_t   g_base    = 0;
static unsigned long long* g_ret = nullptr;  // core-0 retired guest-instruction count (verify gate); PINNED zero-copy
#if RVCUD_HOTHIST
static unsigned long long* g_hot_host = nullptr;   // managed mirror of the per-uop execution histogram
#endif
// Translate-on-miss state (host mirrors, so an untranslated indirect-jump target can be translated
// lazily and appended without re-doing the whole image). g_uopcap = device capacity in uops.
static std::vector<uint32_t> g_img;          // guest code words (persisted)
static std::vector<uint32_t> g_pc2uop_h;     // host mirror of pc2uop[]
static std::vector<uint32_t> g_w0h, g_w1h, g_u2pch;  // host mirrors of the uop arrays
static std::vector<uint8_t>  g_uwh;
static int        g_uopcap  = 0;

// TIERED exec_block (defined far below): cross-compile a BOUNDED set of statically-reached rv32i words
// to ONE PTX kernel (guest regs → PTX registers, no per-uop interpreter tax), dispatched by pc via
// brx.idx; the interpreter owns everything uncompiled (shared g_state/g_mem, hand off via pc). Declared
// here so cuda_rvcud_set_code / cuda_rvcud_step_all (above the definitions) can use the hybrid path.
static CUmodule   g_xmod    = nullptr;
static CUfunction g_xfn     = nullptr;
static int        g_xblk_ok = 0;
static std::vector<uint8_t> g_xtab;          // per guest-word: 1 if a compiled exec_block entry
static void*      g_x_pc2idx = nullptr;      // device u32[N]: word → branchtargets index, or 0xFFFFFFFF
static void       rvxblk_build();
static long long  rvxblk_step(long long budget);
static int        g_x_sptrust = 0;           // exec PTX was built under the global sp-alignment proof
static int        g_rvx_regw_force = 0;      // nonzero: region-size override for assemble-failure retries
static bool       rvx_sp_writer_ok(uint32_t in);

API int cuda_rv32i_init(int nCores, unsigned int memBytes) {
    g_ncores = nCores;
    cudaError_t e;
    // PINNED zero-copy, not managed: kernels touch CoreState only at launch entry/exit (regs go to
    // shared/PTX registers), but the host reads pc after EVERY launch — and on Windows/WDDM managed
    // memory migrates wholesale at every launch/sync boundary (~0.5 ms per step_all call measured).
    if ((e = cudaHostAlloc((void**)&g_state, (size_t)nCores * sizeof(CoreState), cudaHostAllocMapped)) != cudaSuccess) return (int)e;
    // +8 guard bytes: the exec_block's branch-free funnel load reads the aligned word PAIR
    // overlapping a (possibly misaligned) guest address — the high word of the last in-bounds
    // access lands just past the buffer end.
    if ((e = cudaMalloc(&g_mem, (size_t)nCores * memBytes + 8)) != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    cudaMemset(g_mem, 0, (size_t)nCores * memBytes + 8);
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}

API void cuda_rv32i_set_reg  (int core, int i, unsigned int v) {
    if (!i) return;
    g_state[core].regs[i & 31] = v;
    if ((i & 31) == 2 && (v & 3) && g_x_sptrust) {   // unaligned host-set sp voids the sp-alignment proof
        g_xblk_ok = 0; g_x_sptrust = 0;
        fprintf(stderr,"[xblk] exec disabled: host set sp=0x%08X (unaligned) under the sp-alignment proof\n", v);
    }
}
API void cuda_rv32i_set_entry(int core, unsigned int pc)       { g_state[core].pc = pc; }
API void cuda_rv32i_set_halted(int core, int v) { if (v) g_state[core].pc |= HALT_BIT; else g_state[core].pc &= ~HALT_BIT; }
API unsigned int cuda_rv32i_get_pc(int core)  { return g_state ? g_state[core].pc : 0u; }
API unsigned int cuda_rv32i_get_reg(int core, int i) { return g_state ? g_state[core].regs[i & 31] : 0u; }

// Host I/O scatters/gathers words into the interleaved layout (one word per 2D
// "row", stride g_ncores words). off/len are word-aligned for all call sites.
// ncores==1 ⇒ the layout is LINEAR: use a plain cudaMemcpy — the 2D path with 4-byte
// rows is a 64,000-descriptor strided transfer for a 256 KB framebuffer read and
// dominated the per-StepN host cost (~3.8 ms/batch measured end-to-end).
API int cuda_rv32i_write_mem(int core, const void* src, unsigned int off, unsigned int len) {
    if (g_ncores == 1)
        return (int)cudaMemcpy((uint8_t*)g_mem + off, src, len, cudaMemcpyHostToDevice);
    uint8_t* dst = (uint8_t*)(g_mem + (size_t)(off >> 2) * g_ncores + core);
    return (int)cudaMemcpy2D(dst, (size_t)g_ncores * 4, src, 4, 4, len >> 2, cudaMemcpyHostToDevice);
}
API int cuda_rv32i_read_mem(int core, void* dst, unsigned int off, unsigned int len) {
    if (g_ncores == 1)
        return (int)cudaMemcpy(dst, (const uint8_t*)g_mem + off, len, cudaMemcpyDeviceToHost);
    const uint8_t* src = (const uint8_t*)(g_mem + (size_t)(off >> 2) * g_ncores + core);
    return (int)cudaMemcpy2D(dst, 4, src, (size_t)g_ncores * 4, 4, len >> 2, cudaMemcpyDeviceToHost);
}

// ── Staged-MMIO mailbox ─────────────────────────────────────────────────────
// One pinned zero-copy page replaces the ~12 tiny synchronous cudaMemcpys the host used to issue
// per StepN for input staging and output draining — each of those costs a WDDM submit+flush
// (~10-20 µs), together more than the 256 KB framebuffer copy. The host writes the INBOX and reads
// the OUTBOX as plain memory; two trivial kernels scatter/gather the guest cells on-stream around
// the main launch (FIFO order on the default stream makes stage→main→drain sequencing free).
// Single-core (flat memory) only; multi-core keeps the legacy memcpy path. Word layout:
//   [0..15]  cell addresses, set once by the host:
//            0 kbd, 1 mouse, 2 rtc, 3 mtime, 4 uart_head, 5 midi_wr, 6 audio, 7 vsync,
//            8 fbaddr, 9 exit, 10 midi_legacy
//   INBOX    16 kbd_has, 17 kbd_sc, 18 kbd_mods, 19 mouse_has, 20 mdx, 21 mdy, 22 mbtn,
//            23..30 rtc page, 31..32 mtime lo/hi,
//            33 audio_ack_req, 34 vsync_clear_req, 35 midi_legacy_clear_req
//   OUTBOX   48 uart_head, 49 midi_wr, 50..57 audio ctrl page, 58 vsync, 59 fbaddr, 60 exit,
//            61 kbd_status, 62 mouse_status, 63 midi_legacy
static uint32_t* g_iobox = nullptr;   // pinned mapped (UVA: same pointer host & device)
__global__ void rv_iostage_k(uint32_t* mem, uint32_t* box) {
    uint32_t kbd = box[0] >> 2, mouse = box[1] >> 2, rtc = box[2] >> 2, mt = box[3] >> 2;
    for (int i = 0; i < 8; i++) mem[rtc + i] = box[23 + i];
    mem[mt] = box[31]; mem[mt + 1] = box[32];
    mem[kbd + 2] = box[18];                                    // modifiers: always fresh
    if (box[16]) { mem[kbd] = 1; mem[kbd + 1] = box[17]; box[16] = 0; }   // host staged only if free
    if (box[19]) { mem[mouse] = 1; mem[mouse + 1] = box[20];
                   mem[mouse + 2] = box[21]; mem[mouse + 3] = box[22]; box[19] = 0; }
    if (box[33]) { mem[box[6] >> 2]  = 0; box[33] = 0; }       // audio buffer ack (guest awaits 0)
    if (box[34]) { mem[box[7] >> 2]  = 0; box[34] = 0; }       // vsync consume
    if (box[35]) { mem[box[10] >> 2] = 0; box[35] = 0; }       // legacy MIDI cell consume
}
__global__ void rv_iodrain_k(const uint32_t* mem, uint32_t* box) {
    box[48] = mem[box[4] >> 2];
    box[49] = mem[box[5] >> 2];
    uint32_t au = box[6] >> 2;
    for (int i = 0; i < 8; i++) box[50 + i] = mem[au + i];
    box[58] = mem[box[7] >> 2];
    box[59] = mem[box[8] >> 2];
    box[60] = mem[box[9] >> 2];
    box[61] = mem[box[0] >> 2];
    box[62] = mem[box[1] >> 2];
    box[63] = mem[box[10] >> 2];
}
// Returns the pinned mailbox (host pointer) — null when the fast path is unavailable (multi-core).
API void* cuda_rv32i_iobox() {
    if (g_ncores != 1) return nullptr;
    if (!g_iobox) { if (cudaHostAlloc((void**)&g_iobox, 4096, cudaHostAllocMapped) != cudaSuccess) return nullptr;
                    memset((void*)g_iobox, 0, 4096); }
    return (void*)g_iobox;
}
API void cuda_rv32i_iostage() { if (g_iobox) rv_iostage_k<<<1,1>>>(g_mem, g_iobox); }              // async, FIFO before main
API int  cuda_rv32i_iodrain() {
    if (!g_iobox) return -1;
    rv_iodrain_k<<<1,1>>>(g_mem, g_iobox);
    return (int)cudaDeviceSynchronize();
}

// ── Pipelined framebuffer fetch ─────────────────────────────────────────────
// snap(): D2D-snapshot the guest framebuffer into a staging buffer ON THE MAIN STREAM (FIFO order
// makes it race-free: it runs after this batch's drain and before the next batch's kernel), then an
// event-gated async D2H on a second stream copies it into a pinned double buffer WHILE the next
// kernel runs. wait(): block on the OTHER slot (normally long done) and memcpy it out — the host
// thus reads each frame one batch late, with the PCIe copy fully hidden under guest execution.
static void*        g_fbstage = nullptr;  static size_t g_fbstage_sz = 0;
static void*        g_fbpin[2] = {};      static size_t g_fbpin_sz = 0;
static cudaStream_t g_fbs = nullptr;
static cudaEvent_t  g_fbev = nullptr, g_fbdone[2] = {};
API int cuda_rv32i_fb_snap(unsigned int addr, unsigned int len, int slot) {
    if (g_ncores != 1) return -1;
    slot &= 1;
    if (len > g_fbstage_sz) { if (g_fbstage) cudaFree(g_fbstage);
        if (cudaMalloc(&g_fbstage, len) != cudaSuccess) { g_fbstage = nullptr; g_fbstage_sz = 0; return -2; }
        g_fbstage_sz = len; }
    if (len > g_fbpin_sz) { for (int i = 0; i < 2; i++) { if (g_fbpin[i]) cudaFreeHost(g_fbpin[i]);
        if (cudaHostAlloc(&g_fbpin[i], len, 0) != cudaSuccess) { g_fbpin[i] = nullptr; g_fbpin_sz = 0; return -3; }
        memset(g_fbpin[i], 0, len); } g_fbpin_sz = len; }
    if (!g_fbs) { cudaStreamCreateWithFlags(&g_fbs, cudaStreamNonBlocking);
                  cudaEventCreateWithFlags(&g_fbev, cudaEventDisableTiming);
                  for (int i = 0; i < 2; i++) cudaEventCreateWithFlags(&g_fbdone[i], cudaEventDisableTiming); }
    cudaStreamWaitEvent(0, g_fbdone[slot ^ 1], 0);     // don't overwrite the stage while its D2H is in flight
    cudaMemcpyAsync(g_fbstage, (uint8_t*)g_mem + addr, len, cudaMemcpyDeviceToDevice, 0);
    cudaEventRecord(g_fbev, 0);
    cudaStreamWaitEvent(g_fbs, g_fbev, 0);
    cudaMemcpyAsync(g_fbpin[slot], g_fbstage, len, cudaMemcpyDeviceToHost, g_fbs);
    cudaEventRecord(g_fbdone[slot], g_fbs);
    return 0;
}
API int cuda_rv32i_fb_wait(int slot, void* dst, unsigned int len) {
    slot &= 1;
    if (!g_fbpin[slot] || len > g_fbpin_sz) return -1;
    cudaEventSynchronize(g_fbdone[slot]);              // never-recorded events report complete (cold start: zeros)
    memcpy(dst, g_fbpin[slot], len);
    return 0;
}

API int cuda_rv32i_step_all(int budget) {
    if (g_ncores <= 0) return 0;
    // Small (2-warp) blocks: at modest core counts this spreads work across many
    // more SMs than a 256-thread block (which would pile onto a handful of SMs and
    // leave the rest idle) — the interpreter is latency-bound, so SM coverage wins.
    int block = g_ncores < 64 ? g_ncores : 64;
    int grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 33 * sizeof(uint32_t);
    if (g_ncores == 1) rv32i_kernel<true ><<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget);
    else               rv32i_kernel<false><<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget);
    cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
    return le != cudaSuccess ? (int)le : (int)se;
}

// ── rvcud host translator ──────────────────────────────────────────
// Signed immediate assembly (mirrors predecode_kernel, host side).
static inline uint32_t rv_iimm(uint32_t i){ return (uint32_t)((int32_t)i >> 20); }
static inline uint32_t rv_simm(uint32_t i){ return (uint32_t)(((int32_t)(i&0xFE000000u)>>20) | (int32_t)((i>>7)&0x1F)); }
static inline uint32_t rv_bimm(uint32_t i){ return (((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1)
                                                 | ((i&0x80000000u)?0xFFFFE000u:0u); }
static inline uint32_t rv_jimm(uint32_t i){ return (((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1)
                                                 | ((i&0x80000000u)?0xFFE00000u:0u); }
static constexpr uint32_t RC_NOTGT = 0xFFFFFFFFu;

// Translator state shared by the (phase-pluggable) fusion passes.
struct RvcudBuild {
    const uint32_t* img; int nwords; uint32_t base, entry;
    std::vector<uint8_t>  leader;            // any uop start (branch/jump target OR fall-through OR entry)
    std::vector<uint8_t>  xtarget;           // EXPLICIT branch/jump target (foreign entry) — not fall-throughs
    std::vector<uint32_t> liveout;           // size nwords: register bitmask live AFTER each word
    std::vector<uint32_t> pc2uop;            // size nwords
    std::vector<uint32_t> w0, w1, u2pc, tgt; // per-uop (tgt = pending target word for BR/JAL)
    std::vector<uint8_t>  uw;
    std::vector<uint32_t> ext;               // side params for fat fusions (TEXSPAN); uop carries the base index
};

// Backward-dataflow liveness over the guest word stream (for safe fusion: an intermediate
// register may be dropped only if it is dead afterwards). Conservative at JALR (unknown
// target ⇒ all registers live) and treats JAL/branch fall-through as reachable.
static void rvcud_liveness(RvcudBuild& B) {
    int N = B.nwords;
    std::vector<uint32_t> use(N, 0), def(N, 0), livein(N, 0);
    B.liveout.assign(N, 0);
    for (int w = 0; w < N; w++) {
        uint32_t instr = B.img[w], op = instr & 0x7F;
        uint32_t rd = (instr>>7)&0x1F, rs1 = (instr>>15)&0x1F, rs2 = (instr>>20)&0x1F, u = 0, d = 0;
        switch (op) {
            case 0x13: case 0x03: case 0x67: u = (1u<<rs1); d = (1u<<rd); break;          // I / load / jalr
            case 0x33: u = (1u<<rs1)|(1u<<rs2); d = (1u<<rd); break;                       // R-type
            case 0x63: u = (1u<<rs1)|(1u<<rs2); break;                                     // branch
            case 0x23: u = (1u<<rs1)|(1u<<rs2); break;                                     // store
            case 0x37: case 0x17: case 0x6F: d = (1u<<rd); break;                          // lui/auipc/jal
            default: break;
        }
        use[w] = u & ~1u; def[w] = d & ~1u;   // x0 is always 0 → never live
    }
    bool changed = true; int guard = 0;
    while (changed && guard++ < 200) {
        changed = false;
        for (int w = N-1; w >= 0; w--) {
            uint32_t instr = B.img[w], op = instr & 0x7F, pcw = B.base + (uint32_t)w*4, lo = 0;
            if (op == 0x67) lo = 0xFFFFFFFFu;                                              // jalr: target unknown → all live
            else {
                if (w+1 < N) lo |= livein[w+1];                                            // fall-through
                if (op == 0x63 || op == 0x6F) {
                    uint32_t d = (op==0x63) ? rv_bimm(instr) : rv_jimm(instr);
                    uint32_t tw = (pcw + d - B.base) >> 2;
                    if ((int)tw < N) lo |= livein[tw];
                }
            }
            uint32_t li = (lo & ~def[w]) | use[w];
            if (lo != B.liveout[w] || li != livein[w]) { changed = true; B.liveout[w] = lo; livein[w] = li; }
        }
    }
}

// Classify ONE guest word into one uop (the 1:1 base case; fusion passes may
// consume more words and are layered on top in later phases). Returns the number
// of guest words consumed (always 1 here). Fills w0/w1/weight/tgt for the uop.
static int rvcud_classify(const RvcudBuild& B, int w,
                          uint32_t& w0, uint32_t& w1, uint8_t& weight, uint32_t& tgt) {
    const uint32_t instr = B.img[w], pcw = B.base + (uint32_t)w*4;
    const uint32_t op = instr & 0x7F, rd = (instr>>7)&0x1F, f3 = (instr>>12)&7;
    const uint32_t rs1 = (instr>>15)&0x1F, rs2 = (instr>>20)&0x1F, f7 = (instr>>25)&0x7F;
    uint32_t cls = RC_ILL, sra = 0; w1 = 0; weight = 1; tgt = RC_NOTGT;
    switch (op) {
        case 0x13: cls = RC_ALUI; sra = (f3==5 && f7==0x20); w1 = rv_iimm(instr); break;
        case 0x33: if (f7==0x20 && f3==0)      cls = RC_SUB;
                   else if (f7==0x20 && f3==5) { cls = RC_ALUR; sra = 1; }
                   else if (f7==0x01 && f3==0) cls = RC_MULR;       // M-extension MUL (low 32)
                   else if (f7==0x01)          cls = RC_MEXT;       // M-ext mulh/mulhsu/mulhu/div/divu/rem/remu (by f3)
                   else if (f7==0)             cls = RC_ALUR;
                   else                        cls = RC_ILL; break;
        case 0x03: if (f3==0||f3==1||f3==2||f3==4||f3==5) { cls = RC_LOAD; w1 = rv_iimm(instr); } break;
        case 0x63: if (f3!=2 && f3!=3) { cls = RC_BR; tgt = (pcw + rv_bimm(instr) - B.base) >> 2; } break;
        case 0x23: if (f3<=2) { cls = RC_ST; w1 = rv_simm(instr); } break;
        case 0x6F: cls = RC_JAL;  tgt = (pcw + rv_jimm(instr) - B.base) >> 2; break;
        case 0x67: cls = RC_JALR; w1 = rv_iimm(instr); break;
        case 0x37: cls = RC_CONST; w1 = instr & 0xFFFFF000u; break;             // LUI  → baked constant
        case 0x17: cls = RC_CONST; w1 = pcw + (instr & 0xFFFFF000u); break;      // AUIPC→ pc-folded constant
        case 0x0F: cls = RC_NOP; break;
        default:   cls = RC_ILL; break;
    }
    w0 = RCW0(cls, rd, rs1, rs2, f3, sra, RP_UNC, 0);
    return 1;
}

// COUNTED-LOOP FUSION. `addi rc,rc,K` … `b<cc> rc,rX,T` in one block (independent instrs may sit
// between — they must not read/write rc nor write rX, and no jump/foreign-entry between) → emit the
// between-instrs 1:1 then ONE INCBR uop that increments rc and branches on the new rc. The increment
// is "moved down" to the branch; safe because nothing between reads rc. Saves 1 uop per loop iter.
static int rvcud_try_incbr(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    uint32_t i0 = img[w];
    if ((i0 & 0x7F) != 0x13 || ((i0>>12)&7) != 0) return 0;          // addi
    uint32_t rc = (i0>>7)&0x1F, rs1 = (i0>>15)&0x1F;
    if (rc == 0 || rc != rs1) return 0;                              // rc += K (rd==rs1)
    int32_t K = (int32_t)rv_iimm(i0);
    if (K < -128 || K > 127) return 0;                              // K must fit signed 8 bits
    int bj = -1;
    for (int j = w+1; j < N && j <= w+8; j++) {
        if (B.xtarget[j]) return 0;                                  // foreign entry between → unsafe
        uint32_t in = img[j], op = in&0x7F, ird=(in>>7)&0x1F, irs1=(in>>15)&0x1F, irs2=(in>>20)&0x1F;
        if (op == 0x63) {                                            // candidate branch
            uint32_t f3=(in>>12)&7; if (f3==2||f3==3) return 0;
            if (irs1 != rc) return 0;                                // must compare rc
            bj = j; break;
        }
        if (op==0x6F || op==0x67 || op==0x73) return 0;              // jump/system between → bail
        if (irs1==rc || ((op==0x33||op==0x23||op==0x63)&&irs2==rc)) return 0;  // reads rc → bail (increment moves past it)
        if (ird==rc) return 0;                                       // writes rc → bail
    }
    if (bj < 0) return 0;
    uint32_t bin = img[bj], rX=(bin>>20)&0x1F, bf3=(bin>>12)&7;
    for (int j = w+1; j < bj; j++) if (((img[j]>>7)&0x1F)==rX) return 0;  // rX must be stable across the window
    B.pc2uop[w] = ui;                                               // landing at w runs intervening then INCBR (== original)
    for (int j = w+1; j < bj; j++) {
        uint32_t a0,a1,t; uint8_t wt; rvcud_classify(B, j, a0,a1,wt,t);
        B.w0.push_back(a0); B.w1.push_back(a1); B.uw.push_back(wt);
        B.u2pc.push_back(B.base+(uint32_t)j*4); B.tgt.push_back(t); ui++;
    }
    uint32_t tgtw = ((B.base + (uint32_t)bj*4) + rv_bimm(bin) - B.base) >> 2;   // target word index (matches classify)
    B.w0.push_back(RCW0(RC_INCBR, rc, rc, rX, bf3, 0, RP_UNC, 0));
    B.w1.push_back(((uint32_t)(K & 0xFF))<<24);                     // low 24 = target, baked in Pass 3
    B.uw.push_back(2); B.u2pc.push_back(B.base+(uint32_t)bj*4); B.tgt.push_back(tgtw); ui++;
    return bj - w + 1;
}

// AFFINE ×CONST FOLD. A straight-line run of slli/add/sub/addi computes, for its result
// register, a multi-variable affine form  res = imm + Σ_b C_b·(live-in reg b). This is the
// strength-reduced ×constant tree clang emits for `x*K` on rv32i — and it captures the case
// single-root tracking never could: clang reuses the source reg mid-tree AND interleaves the
// "+const" as a *register* add (e.g. `x = a[i]*K + cst + x`). We collapse the whole run to:
//   one MULADD/MULC (res = root·C [+ first live reg])  +  a short ADD/ADDC chain for the rest.
// Emits uops directly (like ifconv); returns words consumed (0 = no fire). Weight m is carried
// on the LAST uop so the chain retires atomically (no mid-chain budget split → no double-run).
static int rvcud_try_affine(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    { uint32_t op0 = img[w] & 0x7F, f3 = (img[w]>>12)&7;             // must start with an affine op
      bool aff0 = (op0==0x13 && (f3==0||f3==1)) || (op0==0x33 && ((img[w]>>25)&0x7F)<=0x20 && f3==0);
      if (!aff0) return 0; }
    static const int NB = 32;
    int64_t co[32][NB]; int64_t im[32]; bool sdef[32]; bool poison[32];
    for (int i=0;i<32;i++){ sdef[i]=false; poison[i]=false; im[i]=0; for(int b=0;b<NB;b++) co[i][b]=0; }
    int64_t tc[NB], ti, s1c[NB], s1a, s2c[NB], s2a;
    auto getsym = [&](uint32_t r, int64_t* c, int64_t& a)->bool {
        for (int b=0;b<NB;b++) c[b]=0; a=0;
        if (r == 0) return true;                                     // x0 == 0
        if (sdef[r]) { for (int b=0;b<NB;b++) c[b]=co[r][b]; a=im[r]; return true; }
        c[r] = 1; return true;                                       // live-in → basis e_r
    };
    // Scan the run. slli/add/sub/addi fold symbolically; OTHER reg-ALU ops (0x13/0x33 — sltiu, the
    // loop-exit compare clang interleaves into the tree) are SURVIVORS: their dest reg is invalidated
    // (fresh basis = its runtime value at the fold's position) and the scan continues. Memory /
    // control / upper-imm / system / a leader END the run. We emit in PROGRAM ORDER below, so a
    // survivor reads/writes happen exactly where they did originally.
    //
    // POISON (correctness backbone): when a non-affine op writes reg R, R's symbol is reused for a
    // *fresh* basis e_R — but the value the emitted MULC/MULADD will read from regs[R] at the fold's
    // position is whatever the survivors leave there (or the live-in, if the write is absorbed), NOT
    // necessarily that post-op value the affine referenced. So a basis built atop a non-affine write
    // is semantically ambiguous. We mark such regs poison[R]=true, propagate it through every fold
    // (poison[rd] = OR of sources), and refuse any candidate whose result OR any of whose basis regs
    // is poisoned. This is what makes the fold provably correct without a runtime self-verify pass.
    // Candidates: each time a fold produces a CLEAN multiply form (one root·C with C not a power of
    // two — pure shifts stay shifts — plus coef-1 units + imm) for a live-out reg, snapshot it. The
    // reg may be clobbered later (clang reuses regs), so we record the classification AT its def.
    struct Cand { int reg, pos, rootv, C, nu, unit[8]; int64_t K; };
    Cand cands[32]; int nc = 0;
    auto classify = [&](int reg)->void {                             // record co[reg] if a clean ×const
        if (nc >= 32) return;
        if (poison[reg]) return;                                     // chain touched a non-affine write → basis invalid
        int rv=-1, un[8], n=0; uint32_t cc=0; bool clean=true;
        for (int b=1;b<NB;b++){ int64_t c=co[reg][b]; if(!c) continue;
            if (poison[b]) { clean=false; break; }                   // a basis reg was re-defined non-affinely → e_b overloaded
            if (c==1) { if (n>=8){clean=false;break;} un[n++]=b; }
            else if (rv<0){ rv=b; cc=(uint32_t)c; } else { clean=false; break; } }
        if (!clean || rv<0 || cc==0 || cc==1 || (cc & (cc-1))==0) return;   // need genuine ×C (non-pow2)
        Cand& cd = cands[nc++]; cd.reg=reg; cd.rootv=rv; cd.C=(int)cc; cd.nu=n; cd.K=im[reg];
        for (int k=0;k<n;k++) cd.unit[k]=un[k];
    };
    for (int j = w; j < N; j++) {
        if (j > w && B.leader[j]) break;
        uint32_t instr = img[j], op = instr & 0x7F, f3 = (instr>>12)&7, f7 = (instr>>25)&0x7F;
        uint32_t rd = (instr>>7)&0x1F, rs1 = (instr>>15)&0x1F, rs2 = (instr>>20)&0x1F;
        if (op==0x03||op==0x23||op==0x63||op==0x6F||op==0x67||op==0x37||op==0x17||op==0x73||op==0x0F) break;
        bool folded = false;
        if (op==0x13 && f3==1 && rd) {                               // slli rd, rs1, sh
            uint32_t sh=(instr>>20)&0x1F; getsym(rs1,s1c,s1a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]<<sh; im[rd]=s1a<<sh; folded=true;
            poison[rd]=poison[rs1];
        } else if (op==0x33 && f7==0x00 && f3==0 && rd) {            // add
            getsym(rs1,s1c,s1a); getsym(rs2,s2c,s2a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]+s2c[b]; im[rd]=s1a+s2a; folded=true;
            poison[rd]=poison[rs1]||poison[rs2];
        } else if (op==0x33 && f7==0x20 && f3==0 && rd) {            // sub
            getsym(rs1,s1c,s1a); getsym(rs2,s2c,s2a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]-s2c[b]; im[rd]=s1a-s2a; folded=true;
            poison[rd]=poison[rs1]||poison[rs2];
        } else if (op==0x13 && f3==0 && rd) {                        // addi (mv when K==0)
            getsym(rs1,s1c,s1a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]; im[rd]=s1a+(int64_t)(int32_t)rv_iimm(instr); folded=true;
            poison[rd]=poison[rs1];
        }
        if (folded) { sdef[rd]=true; if ((B.liveout[j]>>rd)&1u) { int prev=nc; classify(rd); if (nc>prev) cands[nc-1].pos=j; } }
        else if (rd) { sdef[rd]=false; poison[rd]=true; }            // non-affine write → fresh basis, poisoned
        (void)tc; (void)ti;
    }
    if (nc == 0) return 0;
    auto push = [&](uint32_t a0, uint32_t a1, uint8_t wt){
        B.w0.push_back(a0); B.w1.push_back(a1); B.uw.push_back(wt);
        B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++; };
    // Try candidates by DESC pos (largest collapse first). The first whose backward-slice validates wins.
    for (;;) {
        int pick = -1; for (int i=0;i<nc;i++) if (pick<0 || cands[i].pos > cands[pick].pos) pick=i;
        if (pick < 0) break;
        Cand cd = cands[pick]; cands[pick].pos = -1;                 // consume (won't pick again)
        if (cd.pos < 0) break;
        int res=cd.reg, rootv=cd.rootv, nu=cd.nu; uint32_t C=(uint32_t)cd.C; int64_t K=cd.K;
        int runEnd = cd.pos, m = runEnd - w + 1;
        if (m < 3 || m > 200) continue;
        // Backward-slice [w..runEnd]: SURVIVOR if dest feeds a live-out reg (≠ res, delivered by fold).
        bool surv[201]; for (int t=0;t<m;t++) surv[t]=false;
        uint32_t need = B.liveout[runEnd] & ~(1u<<res);
        for (int t = m-1; t >= 0; t--) {
            uint32_t in=img[w+t], op=in&0x7F, d=(in>>7)&0x1F, a=(in>>15)&0x1F, b2=(in>>20)&0x1F;
            if (d && ((need>>d)&1u)) { surv[t]=true; need &= ~(1u<<d); need |= (1u<<a); if (op==0x33) need |= (1u<<b2); }
        }
        // Validate: no survivor writes res / a fold-basis reg, nor reads res (program-order safety).
        auto isBasis = [&](uint32_t r){ if((int)r==rootv) return true; for(int k=0;k<nu;k++) if((int)r==cd.unit[k]) return true; return false; };
        bool ok = true;
        for (int t = 0; t < m && ok; t++) { if (!surv[t]) continue;
            uint32_t in=img[w+t], op=in&0x7F, d=(in>>7)&0x1F, a=(in>>15)&0x1F, b2=(in>>20)&0x1F;
            if (d==(uint32_t)res || isBasis(d)) ok=false;
            if (a==(uint32_t)res || (op==0x33 && b2==(uint32_t)res)) ok=false;
        }
        if (!ok) continue;
        int first = -1; for (int k=0;k<nu;k++) if (cd.unit[k]==res) { first=k; break; }
        if (first < 0 && nu > 0) first = 0;
        int nuops = 1 + (nu>0 ? nu-1 : 0) + (K!=0 ? 1 : 0);
        B.pc2uop[w] = ui;
        // Emit in PROGRAM ORDER: survivors 1:1 where they sit; fold chain at res's def; skip absorbed.
        for (int t = 0; t < m; t++) { int j = w + t;
            if (j == runEnd) {
                int emitted = 0;
                if (first >= 0) push(RCW0(RC_MULADD,(uint32_t)res,(uint32_t)rootv,(uint32_t)cd.unit[first],0,0,RP_UNC,0), C, 0);
                else            push(RCW0(RC_MULC,  (uint32_t)res,(uint32_t)rootv,0,0,0,RP_UNC,0), C, 0);
                emitted++;
                for (int k = 0; k < nu; k++) { if (k == first) continue;
                    uint8_t wt = (++emitted == nuops) ? (uint8_t)(m>255?255:m) : 0;
                    push(RCW0(RC_ALUR,(uint32_t)res,(uint32_t)res,(uint32_t)cd.unit[k],0,0,RP_UNC,0), 0, wt); }
                if (K != 0) push(RCW0(RC_ADDC,(uint32_t)res,(uint32_t)res,0,0,0,RP_UNC,0), (uint32_t)K, (uint8_t)(m>255?255:m));
                if (nuops == 1) B.uw[B.uw.size()-1] = (uint8_t)(m>255?255:m);
            } else if (surv[t]) {
                uint32_t a0,a1,tg; uint8_t wt; rvcud_classify(B, j, a0,a1,wt,tg); push(a0,a1,0);
            }
        }
        return m;
    }
    return 0;
}

// Fusion hook. A fused uop may consume several words but MUST NOT cross or swallow a
// leader (interior words must be non-leaders), and dropped intermediates must be dead.
static int rvcud_try_fuse(const RvcudBuild& B, int w,
                          uint32_t& w0, uint32_t& w1, uint8_t& weight, uint32_t& tgt) {
    const int N = B.nwords;
    const uint32_t* img = B.img;
    auto OPC = [&](int j){ return img[j] & 0x7F; };

    // (affine ×const fold moved to rvcud_try_affine, which can emit a multi-uop chain)

    // ── XSH — xorshift step: (slli|srli) rt,rs,k ; xor rd,{rt,rs} (rt dead after) → rd = rs ^ (rs<<|>>k) ──
    if (OPC(w)==0x13 && w+1<N && !B.leader[w+1] && OPC(w+1)==0x33) {
        uint32_t i1=img[w], i2=img[w+1];
        uint32_t sf3=(i1>>12)&7, sf7=(i1>>25)&0x7F;
        bool left = (sf3==1), right = (sf3==5 && sf7==0);             // slli or srli (logical) — not srai
        uint32_t rt=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, sh=(i1>>20)&0x1F;
        uint32_t xf3=(i2>>12)&7, xf7=(i2>>25)&0x7F, rd=(i2>>7)&0x1F, ra=(i2>>15)&0x1F, rb=(i2>>20)&0x1F;
        if ((left||right) && xf3==4 && xf7==0 && rt!=0 &&            // xor rd, ra, rb
            ((ra==rt && rb==rs) || (ra==rs && rb==rt))) {            // operands are {shifted rt, original rs}
            bool rtdead = (rd==rt) || !(B.liveout[w+1] & (1u<<rt));  // the shift temp must not escape
            if (rs!=rt && rtdead) {
                w0 = RCW0(RC_XSH, rd, rs, 0, 0, (uint32_t)right, RP_UNC, 0);   // sra bit = right-shift
                w1 = sh; weight = 2; tgt = RC_NOTGT;
                return 2;
            }
        }
    }

    // ── Phase 2: LEA — slli rt,rs,k ; add rd,ra,rb  (rt an operand, dead after) → (rs<<k)+rother ──
    if (OPC(w)==0x13 && ((img[w]>>12)&7)==1 && w+1<N && !B.leader[w+1] && OPC(w+1)==0x33) {
        uint32_t i1=img[w], i2=img[w+1];
        uint32_t rt=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, sh=(i1>>20)&0x1F;
        uint32_t f3b=(i2>>12)&7, f7b=(i2>>25)&0x7F, rd=(i2>>7)&0x1F, ra=(i2>>15)&0x1F, rb=(i2>>20)&0x1F;
        if (f3b==0 && f7b==0 && rt!=0 && (ra==rt || rb==rt)) {
            uint32_t rother = (ra==rt) ? rb : ra;
            bool rtdead = (rd==rt) || !(B.liveout[w+1] & (1u<<rt));   // rt's slli value must not escape
            if (rother != rt && rtdead) {
                w0 = RCW0(RC_LEA, rd, rs, rother, 0,0,RP_UNC,0);
                w1 = sh; weight = 2; tgt = RC_NOTGT;
                return 2;
            }
        }
    }

    // ── Phase 2: CONST — lui/auipc rd ; addi rd,rd,imm → one baked 32-bit constant ──
    if ((OPC(w)==0x37 || OPC(w)==0x17) && w+1<N && !B.leader[w+1]
        && OPC(w+1)==0x13 && ((img[w+1]>>12)&7)==0) {
        uint32_t i1=img[w], i2=img[w+1];
        uint32_t rd1=(i1>>7)&0x1F, rd2=(i2>>7)&0x1F, rs=(i2>>15)&0x1F;
        if (rd1!=0 && rd1==rd2 && rs==rd1) {
            uint32_t up = (OPC(w)==0x37) ? (i1 & 0xFFFFF000u)
                                         : ((B.base + (uint32_t)w*4) + (i1 & 0xFFFFF000u));
            w0 = RCW0(RC_CONST, rd1, 0,0,0,0,RP_UNC,0);
            w1 = up + rv_iimm(i2); weight = 2; tgt = RC_NOTGT;
            return 2;
        }
    }

    return rvcud_classify(B, w, w0, w1, weight, tgt);   // 1:1 fallback
}

// Phase 4 (T2) — IF-CONVERSION. A short FORWARD conditional branch guarding 1..4 straight-line
// convertible instructions becomes: a SETP uop (P = branch condition) followed by the body as
// GUARD_NOTP uops (live when the branch is NOT taken — RISC-V skips the body when taken). The
// branch — and its warp divergence — is gone. Pushes uops directly; returns words consumed (0 if
// no match). Stores are predicated; loads run speculatively (safe under flat fault-free memory).
static int rvcud_try_ifconv(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    uint32_t instr = img[w];
    if ((instr & 0x7F) != 0x63) return 0;
    uint32_t f3 = (instr>>12)&7; if (f3==2 || f3==3) return 0;
    uint32_t bimm = rv_bimm(instr); if ((int32_t)bimm <= 0) return 0;          // forward only
    uint32_t pcw = B.base + (uint32_t)w*4;
    int tw = (int)((pcw + bimm - B.base) >> 2), n = tw - w - 1;
    if (n < 1 || n > 4 || tw >= N) return 0;
    for (int k = 1; k <= n; k++) {
        int j = w + k; if (B.xtarget[j]) return 0;                             // no foreign edge into the body
        uint32_t op = img[j]&0x7F, jf3=(img[j]>>12)&7, jf7=(img[j]>>25)&0x7F; bool okc = false;
        if      (op==0x13) okc = !((jf3==1 && jf7!=0) || (jf3==5 && jf7!=0 && jf7!=0x20));
        else if (op==0x33) okc = (jf7==0) || (jf7==0x20 && (jf3==0||jf3==5));
        else if (op==0x03) okc = (jf3==0||jf3==1||jf3==2||jf3==4||jf3==5);
        else if (op==0x23) okc = (jf3<=2);
        else if (op==0x37 || op==0x17) okc = true;
        if (!okc) return 0;                                                    // jump/branch/fence/illegal → abort
    }
    uint32_t rs1=(instr>>15)&0x1F, rs2=(instr>>20)&0x1F;
    B.pc2uop[w] = ui;
    B.w0.push_back(RCW0(RC_NOP, 0, rs1, rs2, f3, 0, RP_SETP, 0));              // SETP: P = branch condition
    B.w1.push_back(0); B.uw.push_back(1); B.u2pc.push_back(pcw); B.tgt.push_back(RC_NOTGT); ui++;
    for (int k = 1; k <= n; k++) {
        int j = w + k; uint32_t a0,a1,t; uint8_t wt; rvcud_classify(B, j, a0,a1,wt,t);
        a0 = (a0 & ~((uint32_t)7u<<26)) | ((uint32_t)RP_GNP<<26);              // GUARD_NOTP body
        B.pc2uop[j] = ui;
        B.w0.push_back(a0); B.w1.push_back(a1); B.uw.push_back(1);
        B.u2pc.push_back(B.base + (uint32_t)j*4); B.tgt.push_back(t); ui++;
    }
    return n + 1;
}

// STRIDED COPY LOOP → COPYLOOPS. A byte/half/word copy step (lX rt,0(src); sX rt,0(dst); addi src,Ks;
// addi dst,Kd — any order) immediately followed by `bne cnt,lim,→w` (back-edge) runs the WHOLE strided
// copy in one native loop uop. Generalizes the per-step COPYPI to its loop form for arbitrary element size
// and strides (e.g. column blit: src+=1, dst+=row pitch). Captures patch/column blit loops. Params in
// g_ext (strides exceed a uint2 with the regs). Consumes 5 words; weight 5/iter.
static int rvcud_try_copyloops(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+5 >= N) return 0;                                          // 4 body words + bne + a fall-through instr
    uint32_t i0 = img[w], lf3 = (i0>>12)&7;
    if ((i0 & 0x7F) != 0x03 || !(lf3==0||lf3==1||lf3==2||lf3==4||lf3==5) || rv_iimm(i0) != 0) return 0;  // load rt,0(src)
    uint32_t rt=(i0>>7)&0x1F, src=(i0>>15)&0x1F;
    if (rt == 0 || rt == src) return 0;
    int stPos=-1, srcPos=-1, dstPos=-1; uint32_t dst=0, dstReg=0; int32_t Ks=0, Kd=0;
    for (int j = w+1; j <= w+3; j++) {
        if (B.leader[j]) return 0;
        uint32_t in = img[j], op = in & 0x7F;
        if (op == 0x23) {                                            // store rt,0(dst)
            uint32_t sf3=(in>>12)&7, srt=(in>>20)&0x1F, sba=(in>>15)&0x1F;
            if (stPos>=0 || srt != rt || rv_simm(in) != 0) return 0;
            bool okw = ((lf3==0||lf3==4)&&sf3==0) || ((lf3==1||lf3==5)&&sf3==1) || (lf3==2&&sf3==2);
            if (!okw) return 0; dst = sba; stPos = j;
        } else if (op == 0x13 && ((in>>12)&7) == 0) {                // addi r,r,K (pointer bump)
            uint32_t ard=(in>>7)&0x1F, ars1=(in>>15)&0x1F; int32_t k=(int32_t)rv_iimm(in);
            if (ard != ars1 || ard == 0 || k < -32768 || k > 32767) return 0;
            if (ard == src) { if (srcPos>=0) return 0; Ks = k; srcPos = j; }
            else            { if (dstPos>=0) return 0; Kd = k; dstReg = ard; dstPos = j; }
        } else return 0;
    }
    if (stPos<0 || srcPos<0 || dstPos<0 || dstReg != dst) return 0;
    if (stPos > dstPos) return 0;                                    // post-increment (store reads dst pre-bump)
    if (dst == 0 || dst == src || dst == rt) return 0;
    uint32_t br = img[w+4];                                          // require `bne cnt,lim, →w`
    if ((br & 0x7F) != 0x63 || ((br>>12)&7) != 1) return 0;
    uint32_t bpc = B.base + (uint32_t)(w+4)*4, tw = (bpc + rv_bimm(br) - B.base) >> 2;
    if ((int)tw != w) return 0;                                      // back-edge to the loop header
    uint32_t b1=(br>>15)&0x1F, b2=(br>>20)&0x1F, creg, lim;
    if      (b1==src || b1==dst) { creg=b1; lim=b2; }
    else if (b2==src || b2==dst) { creg=b2; lim=b1; }
    else return 0;
    if (lim==0 || lim==src || lim==dst || lim==rt) return 0;         // limit reg distinct
    uint32_t e = (uint32_t)B.ext.size();
    B.ext.push_back((src&31)|((dst&31)<<5)|((rt&31)<<10)|((lim&31)<<15)|((uint32_t)(creg==dst?1:0)<<20)|((lf3&7)<<21));
    B.ext.push_back(((uint32_t)Ks & 0xFFFF) | (((uint32_t)Kd & 0xFFFF) << 16));
    B.pc2uop[w] = ui;
    B.w0.push_back(RC_COPYLOOPS | (e<<7));
    B.w1.push_back(0);                                              // fall-through baked in Pass 3 (full w1)
    B.uw.push_back(5); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back((uint32_t)(w+5)); ui++;
    return 5;
}

// MEMORY-COPY FUSION. A byte/half/word copy step `lb rt,0(src); sb rt,0(dst); addi src,Ks; addi dst,Kd`
// (the four in ANY order — clang interleaves the pointer bumps differently for horizontal memcpy vs the
// vertical column blit) collapses to ONE uop: mem[dst]=mem[src]; src+=Ks; dst+=Kd. Requires offset 0 on
// both, rt dead after the store, and src/dst/rt distinct. Replaces 4 rv32i instrs/iter with 1 — blit-style
// hot paths are exactly these copy loops (span blits + column blits). Strides fit signed 16 bits.
static int rvcud_try_copy(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    uint32_t i0 = img[w], lf3 = (i0>>12)&7;
    if ((i0 & 0x7F) != 0x03) return 0;                               // must start with a load
    if (!(lf3==0||lf3==1||lf3==2||lf3==4||lf3==5) || rv_iimm(i0) != 0) return 0;
    uint32_t rt = (i0>>7)&0x1F, src = (i0>>15)&0x1F;
    if (rt == 0 || rt == src) return 0;
    int stPos=-1, srcPos=-1, dstPos=-1; uint32_t dst=0, dstReg=0; int32_t Ks=0, Kd=0;
    for (int j = w+1; j <= w+3 && j < N; j++) {
        if (B.leader[j]) return 0;                                   // a foreign entry splits the run
        uint32_t in = img[j], op = in & 0x7F;
        if (op == 0x23) {                                            // the store of rt
            uint32_t sf3=(in>>12)&7, srt=(in>>20)&0x1F, sba=(in>>15)&0x1F;
            if (stPos>=0 || srt != rt || rv_simm(in) != 0) return 0;
            bool okw = ((lf3==0||lf3==4)&&sf3==0) || ((lf3==1||lf3==5)&&sf3==1) || (lf3==2&&sf3==2);
            if (!okw) return 0;                                      // load/store widths must match
            dst = sba; stPos = j;
        } else if (op == 0x13 && ((in>>12)&7) == 0) {                // a pointer bump (addi r,r,K)
            uint32_t ard=(in>>7)&0x1F, ars1=(in>>15)&0x1F; int32_t k=(int32_t)rv_iimm(in);
            if (ard != ars1 || ard == 0 || k < -32768 || k > 32767) return 0;
            if (ard == src) { if (srcPos>=0) return 0; Ks = k; srcPos = j; }
            else            { if (dstPos>=0) return 0; Kd = k; dstReg = ard; dstPos = j; }
        } else return 0;                                             // anything else → not a clean copy step
    }
    if (stPos<0 || srcPos<0 || dstPos<0 || dstReg != dst) return 0;  // the non-src bump must target the store base
    if (dst == 0 || dst == src || dst == rt) return 0;
    if (stPos > dstPos) return 0;             // post-increment ONLY: the store must read dst BEFORE its addi
                                              // (the src bump's position is irrelevant — the load at w already read src)
    B.pc2uop[w] = ui;                         // rt written by the uop (mirrors the load), so no liveness check needed
    B.w0.push_back(RCW0(RC_COPYPI, rt, src, dst, lf3, 0, RP_UNC, 0));
    B.w1.push_back(((uint32_t)Ks & 0xFFFF) | ((uint32_t)Kd << 16));  // src stride lo16, dst stride hi16
    B.uw.push_back(4); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
    return 4;
}

// MEMSET LOOP → one native byte-fill loop. clang/libc memset/bzero inner loop:
//   addi nD,DST,1 ; sb VAL,0(DST) ; mv DST,nD ; bne nD,END,→top   →  fill mem[DST..END) with VAL, DST→END.
// Universal (every program memsets). VAL,END loop-invariant. Final state reproduced exactly (DST=nD=END),
// so no liveness check needed. Consumes 4 words; weight 4/iter.
static int rvcud_try_memset(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+4 >= N) return 0;                                          // 4 body words + a fall-through instr
    uint32_t i0=img[w],i1=img[w+1],i2=img[w+2],i3=img[w+3];
    if ((i0&0x7F)!=0x13 || ((i0>>12)&7)!=0 || (int32_t)rv_iimm(i0)!=1) return 0;        // addi nD,DST,1
    uint32_t nD=(i0>>7)&0x1F, DST=(i0>>15)&0x1F;
    if (nD==0 || DST==0 || nD==DST) return 0;
    if (B.leader[w+1]||B.leader[w+2]||B.leader[w+3]) return 0;
    if ((i1&0x7F)!=0x23 || ((i1>>12)&7)!=0 || ((i1>>15)&0x1F)!=DST || rv_simm(i1)!=0) return 0;  // sb VAL,0(DST)
    uint32_t VAL=(i1>>20)&0x1F;
    if ((i2&0x7F)!=0x13 || ((i2>>12)&7)!=0 || ((i2>>7)&0x1F)!=DST || ((i2>>15)&0x1F)!=nD || rv_iimm(i2)!=0) return 0;  // mv DST,nD
    if ((i3&0x7F)!=0x63 || ((i3>>12)&7)!=1) return 0;                                   // bne nD,END,→top
    uint32_t b1=(i3>>15)&0x1F, b2=(i3>>20)&0x1F, END;
    if      (b1==nD) END=b2;
    else if (b2==nD) END=b1;
    else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+3)*4, tw=(bpc+rv_bimm(i3)-B.base)>>2;
    if ((int)tw != w) return 0;                                     // back-edge to the loop header
    if (VAL==DST || VAL==nD || END==DST || END==nD || VAL==END) return 0;  // VAL,END clean live-ins distinct from temps
    B.pc2uop[w] = ui;
    B.w0.push_back(RCW0(RC_MEMSET, DST, VAL, END, 0, 0, RP_UNC, 0));   // rd=DST, rs1=VAL, rs2=END
    B.w1.push_back((uint32_t)nD << 24);                             // nD in hi byte; low 24 = fall-through idx (Pass 3)
    B.uw.push_back(4); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back((uint32_t)(w+4)); ui++;
    return 4;
}

// DIRECT-FORM FILL LOOP → WORDFILL. The temp-less fill clang emits for word/half block clears:
//   sX VAL,0(D) ; addi D,D,K ; bne D,END,→top   →  fill stride-K with VAL (3 instrs/iter).
// rvcud_try_memset matches the addi-temp byte form; this matches the direct form (profiled hot: a
// sw-based fill at 2 dispatched uops/iter). VAL/END loop-invariant (VAL may be x0 = zero fill); D is
// the only written register, so the closed form is exact. Consumes 3 words; weight 3/iteration.
static int rvcud_try_wordfill(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+3 >= N) return 0;                                          // 3 body words + a fall-through instr
    uint32_t i0=img[w], i1=img[w+1], i2=img[w+2];
    if ((i0&0x7F)!=0x23 || ((i0>>12)&7)>2 || rv_simm(i0)!=0) return 0;          // sb/sh/sw VAL,0(D)
    uint32_t f3=(i0>>12)&7, D=(i0>>15)&0x1F, VAL=(i0>>20)&0x1F;
    if (B.leader[w+1] || B.leader[w+2]) return 0;
    if ((i1&0x7F)!=0x13 || ((i1>>12)&7)!=0 || ((i1>>7)&0x1F)!=D || ((i1>>15)&0x1F)!=D) return 0;  // addi D,D,K
    int32_t K=(int32_t)rv_iimm(i1); if (K<-128||K>127||K==0) return 0;          // K fits w1's signed hi byte
    if ((i2&0x7F)!=0x63 || ((i2>>12)&7)!=1) return 0;                           // bne D,END,→top
    uint32_t b1=(i2>>15)&0x1F, b2=(i2>>20)&0x1F, END;
    if (b1==D) END=b2; else if (b2==D) END=b1; else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+2)*4, tw=(bpc+rv_bimm(i2)-B.base)>>2;
    if ((int)tw != w) return 0;                                                 // back-edge to the loop header
    if (D==0 || VAL==D || END==D || END==0) return 0;                           // VAL may be x0
    B.pc2uop[w] = ui;
    B.w0.push_back(RCW0(RC_WORDFILL, D, VAL, END, f3, 0, RP_UNC, 0));           // rd=D, rs1=VAL, rs2=END
    B.w1.push_back(((uint32_t)(K & 0xFF)) << 24);                               // K hi byte; low 24 = fall-through (Pass 3)
    B.uw.push_back(3); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+3)); ui++;
    return 3;
}

// COUNTED SEARCH LOOP → WORDSCAN. The record-table scan shape (compare a key against fixed-stride
// records; profiled at ~15% of uop-execs at 4 dispatched uops/iter):
//   addi C,C,Kc ; addi P,P,Kp ; b<cc> C,Z,→EXIT(fwd) ; lw V,off(P) ; bne V,KEY,→top
// → ONE native loop uop (params in g_ext: regs + strides + offset; exit-target baked into ext[e+2]
// in Pass 3, fall-through in w1). Two exits with EXACT per-path weights: exit-branch iter retires 3,
// full iter 5. Matched by structure (any regs/strides/offset/cmp) → generic. Consumes 5 words.
static int rvcud_try_wordscan(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+5 >= N) return 0;                                          // 5 body words + a fall-through instr
    uint32_t i0=img[w],i1=img[w+1],i2=img[w+2],i3=img[w+3],i4=img[w+4];
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    if ((i0&0x7F)!=0x13 || F3(i0)!=0) return 0;                      // addi C,C,Kc
    uint32_t C=RD(i0); if (C==0 || S1(i0)!=C) return 0;
    int32_t Kc=(int32_t)rv_iimm(i0); if (Kc<-128||Kc>127) return 0;
    for (int j=w+1; j<=w+4; j++) if (B.leader[j]) return 0;
    if ((i1&0x7F)!=0x13 || F3(i1)!=0) return 0;                      // addi P,P,Kp
    uint32_t P=RD(i1); if (P==0 || P==C || S1(i1)!=P) return 0;
    int32_t Kp=(int32_t)rv_iimm(i1); if (Kp<-128||Kp>127) return 0;
    if ((i2&0x7F)!=0x63) return 0;                                   // b<cc> C,Z,→EXIT (forward)
    uint32_t cf3=F3(i2); if (cf3==2||cf3==3) return 0;
    uint32_t Z;
    if      (S1(i2)==C) Z=S2(i2);
    else return 0;                                                   // C must be the FIRST operand (cmp polarity fixed)
    if (Z==C || Z==P) return 0;                                      // Z loop-invariant (x0 allowed)
    int32_t bofs=(int32_t)rv_bimm(i2); if (bofs<=0) return 0;        // forward exit only
    uint32_t twx=(uint32_t)((B.base+(uint32_t)(w+2)*4 + (uint32_t)bofs - B.base)>>2);
    if ((int)twx >= N) return 0;
    if ((i3&0x7F)!=0x03 || F3(i3)!=2 || S1(i3)!=P) return 0;         // lw V,off(P)
    uint32_t V=RD(i3); int32_t off=(int32_t)rv_iimm(i3);
    if (V==0 || V==C || V==P || V==Z) return 0;
    if ((i4&0x7F)!=0x63 || F3(i4)!=1) return 0;                      // bne V,KEY,→top
    uint32_t KEY;
    if      (S1(i4)==V) KEY=S2(i4);
    else if (S2(i4)==V) KEY=S1(i4);
    else return 0;
    if (KEY==C || KEY==P || KEY==V) return 0;                        // KEY loop-invariant (x0 allowed)
    uint32_t bpc=B.base+(uint32_t)(w+4)*4, twb=(bpc+rv_bimm(i4)-B.base)>>2;
    if ((int)twb != w) return 0;                                     // back-edge to the loop header
    uint32_t e=(uint32_t)B.ext.size();
    B.ext.push_back((C&31)|((P&31)<<5)|((V&31)<<10)|((KEY&31)<<15)|((Z&31)<<20)|((cf3&7)<<25));
    B.ext.push_back(((uint32_t)Kc&0xFF) | (((uint32_t)Kp&0xFF)<<8) | (((uint32_t)off&0xFFF)<<20));
    B.ext.push_back(twx);                                            // exit-target WORD; Pass 3 rewrites to uop idx
    B.pc2uop[w]=ui;
    B.w0.push_back(RC_WORDSCAN | (e<<7));                            // ext base index in bits[31:7]
    B.w1.push_back(0);                                              // fall-through baked in Pass 3 (full w1)
    B.uw.push_back(5); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+5)); ui++;
    return 5;
}

// TEMP-ADVANCE STRIDED COPY → COPYLOOPT. A strided column copy whose src-advance goes through a
// TEMP, defeating COPYLOOPS (profiled hot at 5 dispatched uops/iter as 1:1 code):
//   lX V,0(S) ; addi S2,S,Ks ; sX V,0(D) ; addi D,D,Kd ; mv S,S2 ; bne S2,LIM,→top
// → ONE native loop uop (params in g_ext; strides signed 16-bit). All four written regs (V,S2,D,S)
// reproduced exactly each iteration; LIM read-only. Matched by structure → generic. Consumes 6 words.
static int rvcud_try_copyloopt(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+6 >= N) return 0;                                          // 6 body words + a fall-through instr
    uint32_t i0=img[w],i1=img[w+1],i2=img[w+2],i3=img[w+3],i4=img[w+4],i5=img[w+5];
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2_=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    uint32_t lf3=F3(i0);
    if ((i0&0x7F)!=0x03 || !(lf3==0||lf3==1||lf3==2||lf3==4||lf3==5) || rv_iimm(i0)!=0) return 0;  // lX V,0(S)
    uint32_t V=RD(i0), S=S1(i0);
    if (V==0 || S==0 || V==S) return 0;
    for (int j=w+1; j<=w+5; j++) if (B.leader[j]) return 0;
    if ((i1&0x7F)!=0x13 || F3(i1)!=0 || S1(i1)!=S) return 0;          // addi S2,S,Ks (next-src into a temp)
    uint32_t S2=RD(i1); int32_t Ks=(int32_t)rv_iimm(i1);
    if (S2==0 || S2==S || S2==V || Ks<-32768 || Ks>32767) return 0;
    if ((i2&0x7F)!=0x23 || S2_(i2)!=V || rv_simm(i2)!=0) return 0;    // sX V,0(D)
    uint32_t sf3=F3(i2), D=S1(i2);
    bool okw = ((lf3==0||lf3==4)&&sf3==0) || ((lf3==1||lf3==5)&&sf3==1) || (lf3==2&&sf3==2);
    if (!okw || D==0 || D==S || D==S2 || D==V) return 0;
    if ((i3&0x7F)!=0x13 || F3(i3)!=0 || RD(i3)!=D || S1(i3)!=D) return 0;  // addi D,D,Kd
    int32_t Kd=(int32_t)rv_iimm(i3); if (Kd<-32768 || Kd>32767) return 0;
    if ((i4&0x7F)!=0x13 || F3(i4)!=0 || RD(i4)!=S || S1(i4)!=S2 || rv_iimm(i4)!=0) return 0;  // mv S,S2
    if ((i5&0x7F)!=0x63 || F3(i5)!=1) return 0;                       // bne S2,LIM,→top
    uint32_t b1=S1(i5), b2=S2_(i5), LIM;
    if (b1==S2) LIM=b2; else if (b2==S2) LIM=b1; else return 0;
    if (LIM==V || LIM==S || LIM==S2 || LIM==D) return 0;              // LIM loop-invariant (x0 allowed)
    uint32_t bpc=B.base+(uint32_t)(w+5)*4, twb=(bpc+rv_bimm(i5)-B.base)>>2;
    if ((int)twb != w) return 0;                                      // back-edge to the loop header
    uint32_t e=(uint32_t)B.ext.size();
    B.ext.push_back((S&31)|((D&31)<<5)|((V&31)<<10)|((S2&31)<<15)|((LIM&31)<<20)|((lf3&7)<<25));
    B.ext.push_back(((uint32_t)Ks&0xFFFF) | (((uint32_t)Kd&0xFFFF)<<16));
    B.pc2uop[w]=ui;
    B.w0.push_back(RC_COPYLOOPT | (e<<7));                              // ext base index in bits[31:7]
    B.w1.push_back(0);                                               // fall-through baked in Pass 3 (full w1)
    B.uw.push_back(6); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+6)); ui++;
    return 6;
}

// MEMCPY LOOP → COPYLOOP. A forward unit-stride BYTE copy step `lbu/lb rt,0(src); sb rt,0(dst);
// addi src,1; addi dst,1` (w..w+3, any order) immediately followed by `bne creg,rlim, →w` (back-edge
// to this loop header) becomes ONE uop that runs the whole copy in-kernel, word-widened (4 bytes/step
// when ≥4 remain and the regions don't overlap within a word). Removes the per-byte dispatch AND ~3/4
// of the memory ops. Consumes 5 words. See IDEA_AdvancedFusion.md §1 for the budget/resume/overlap proof.
static int rvcud_try_copyloop(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+5 >= N) return 0;                                          // need 4 body + bne + a fall-through instr
    uint32_t i0 = img[w], lf3 = (i0>>12)&7;
    if ((i0 & 0x7F) != 0x03 || !(lf3==0||lf3==4) || rv_iimm(i0) != 0) return 0;  // byte load at 0(src)
    uint32_t rt=(i0>>7)&0x1F, src=(i0>>15)&0x1F;
    if (rt == 0 || rt == src) return 0;
    int stPos=-1, srcPos=-1, dstPos=-1; uint32_t dst=0, dstReg=0;
    for (int j = w+1; j <= w+3; j++) {
        if (B.leader[j]) return 0;
        uint32_t in = img[j], op = in & 0x7F;
        if (op == 0x23) {                                            // sb rt,0(dst)
            uint32_t sf3=(in>>12)&7, srt=(in>>20)&0x1F, sba=(in>>15)&0x1F;
            if (stPos>=0 || sf3!=0 || srt!=rt || rv_simm(in)!=0) return 0;
            dst = sba; stPos = j;
        } else if (op == 0x13 && ((in>>12)&7) == 0) {                // addi r,r,1  (unit stride only)
            uint32_t ard=(in>>7)&0x1F, ars1=(in>>15)&0x1F;
            if (ard != ars1 || (int32_t)rv_iimm(in) != 1) return 0;
            if (ard == src) { if (srcPos>=0) return 0; srcPos = j; }
            else            { if (dstPos>=0) return 0; dstReg = ard; dstPos = j; }
        } else return 0;
    }
    if (stPos<0 || srcPos<0 || dstPos<0 || dstReg != dst) return 0;
    if (stPos > dstPos) return 0;                                    // post-increment order (store reads dst pre-addi)
    if (dst == 0 || dst == src || dst == rt) return 0;
    uint32_t br = img[w+4];                                          // require `bne creg,rlim, →w`
    if ((br & 0x7F) != 0x63 || ((br>>12)&7) != 1) return 0;
    uint32_t bpc = B.base + (uint32_t)(w+4)*4, tw = (bpc + rv_bimm(br) - B.base) >> 2;
    if ((int)tw != w) return 0;                                      // back-edge must target the loop header
    uint32_t b1=(br>>15)&0x1F, b2=(br>>20)&0x1F, creg, rlim;
    if      (b1==src || b1==dst) { creg=b1; rlim=b2; }
    else if (b2==src || b2==dst) { creg=b2; rlim=b1; }
    else return 0;
    if (rlim==0 || rlim==src || rlim==dst || rlim==rt) return 0;     // limit reg must be distinct
    B.pc2uop[w] = ui;
    B.w0.push_back(RCW0(RC_COPYLOOP, rt, src, dst, lf3, (creg==dst)?1:0, RP_UNC, 0));
    B.w1.push_back(rlim << 24);                                      // hi byte = limit reg; low 24 = fall-through idx (Pass 3)
    B.uw.push_back(5); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back((uint32_t)(w+5)); ui++;
    return 5;
}

// SCALED-INDEXED LOAD (virtual-register address). `slli T,ra,k ; add T2,T,rb ; lX rd,imm(T2)` → ONE uop:
// rd = mem[(ra<<k) + rb + imm]. The scaled address (T, T2) lives only in CUDA registers inside the uop —
// a "virtual register" beyond the guest's 32 — cutting two shared writes + two shared reads. Universal
// array-indexing idiom (element size 2^k). ra,rb are read at their live-in values exactly as the original
// three instructions did (operands kept distinct from the temps); T/T2 must be dead after (or == rd).
static int rvcud_try_ldxs(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+2 >= N) return 0;
    uint32_t i0 = img[w];
    if ((i0 & 0x7F) != 0x13 || ((i0>>12)&7) != 1) return 0;          // slli T,ra,k
    uint32_t T=(i0>>7)&0x1F, ra=(i0>>15)&0x1F, k=(i0>>20)&0x1F;
    if (T == 0 || B.leader[w+1]) return 0;
    uint32_t i1 = img[w+1];
    if ((i1 & 0x7F) != 0x33 || ((i1>>25)&0x7F) != 0 || ((i1>>12)&7) != 0) return 0;  // add T2, {T, rb}
    uint32_t T2=(i1>>7)&0x1F, a1=(i1>>15)&0x1F, b1=(i1>>20)&0x1F, rb;
    if      (a1==T) rb=b1;
    else if (b1==T) rb=a1;
    else return 0;
    if (B.leader[w+2]) return 0;
    uint32_t i2=img[w+2], op2=i2&0x7F, f3=(i2>>12)&7, base=(i2>>15)&0x1F;
    if (base != T2) return 0;                                       // the mem op must address through T2
    if (ra==T || ra==T2 || rb==T || rb==T2) return 0;               // ra,rb must be clean live-ins (not the temps)
    if (op2 == 0x03 && (f3==0||f3==1||f3==2||f3==4||f3==5)) {        // scaled-indexed LOAD
        uint32_t rd=(i2>>7)&0x1F;
        if (rd == 0) return 0;
        bool T2dead = !((B.liveout[w+2] >> T2) & 1u);
        if (!(T2dead || T2==rd)) return 0;                          // address temp must not escape
        if (T != T2) { bool Tdead = !((B.liveout[w+2] >> T) & 1u); if (!(Tdead || T==rd)) return 0; }
        B.pc2uop[w] = ui;
        B.w0.push_back(RCW0(RC_LDXS, rd, ra, rb, f3, 0, RP_UNC, 0));
        B.w1.push_back(((uint32_t)k << 24) | (rv_iimm(i2) & 0x00FFFFFFu));   // k in hi byte; 12-bit imm sign-ext-to-24 in low
    } else if (op2 == 0x23 && f3 <= 2) {                            // scaled-indexed STORE
        uint32_t rc=(i2>>20)&0x1F;                                  // stored value (carried in the rd field)
        if (rc==T || rc==T2) return 0;                              // stored value must be a clean live-in
        bool T2dead = !((B.liveout[w+2] >> T2) & 1u); if (!T2dead) return 0;   // store writes no reg → temp must die
        if (T != T2) { bool Tdead = !((B.liveout[w+2] >> T) & 1u); if (!Tdead) return 0; }
        B.pc2uop[w] = ui;
        B.w0.push_back(RCW0(RC_STXS, rc, ra, rb, f3, 0, RP_UNC, 0));
        B.w1.push_back(((uint32_t)k << 24) | (rv_simm(i2) & 0x00FFFFFFu));
    } else return 0;
    B.uw.push_back(3); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
    return 3;
}

// INDEXED LOAD/STORE (virtual-register address). `add rt,ra,rb; lX rd,imm(rt)` (or a store) → ONE uop:
// rd = mem[ra+rb+imm] / mem[ra+rb+imm] = rc. The address temp rt never reaches the shared regfile — it
// lives only in a CUDA register inside the uop, cutting one shared write + one shared read (the regfile
// is the per-uop bottleneck). Pervasive in compiled code (every pointer+index access). Correct-by-construction:
// the load/store reads ra,rb (and rc) at their live-in values exactly as the original add+memop did.
static int rvcud_try_ldx(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+1 >= N) return 0;
    uint32_t i0 = img[w];
    if ((i0 & 0x7F) != 0x33 || ((i0>>25)&0x7F) != 0 || ((i0>>12)&7) != 0) return 0;  // add rt,ra,rb (f7=0,f3=0)
    uint32_t rt=(i0>>7)&0x1F, ra=(i0>>15)&0x1F, rb=(i0>>20)&0x1F;
    if (rt == 0 || B.leader[w+1]) return 0;
    uint32_t i1 = img[w+1], op1 = i1 & 0x7F, f3 = (i1>>12)&7, base = (i1>>15)&0x1F;
    if (base != rt) return 0;                                        // the mem op must address through rt
    bool dead = !((B.liveout[w+1] >> rt) & 1u);
    if (op1 == 0x03 && (f3==0||f3==1||f3==2||f3==4||f3==5)) {         // indexed LOAD
        uint32_t rd = (i1>>7)&0x1F;
        if (rd == 0 || (rt != rd && !dead)) return 0;                // rt must be overwritten by rd, or already dead
        B.pc2uop[w] = ui;
        B.w0.push_back(RCW0(RC_LDX, rd, ra, rb, f3, 0, RP_UNC, 0));
        B.w1.push_back(rv_iimm(i1));
    } else if (op1 == 0x23 && f3 <= 2) {                             // indexed STORE
        uint32_t rc = (i1>>20)&0x1F;
        if (!dead || rc == rt) return 0;                             // rt dies here; the stored value can't be rt
        B.pc2uop[w] = ui;
        B.w0.push_back(RCW0(RC_STX, rc, ra, rb, f3, 0, RP_UNC, 0));   // stored value rc carried in the rd field
        B.w1.push_back(rv_simm(i1));
    } else return 0;
    B.uw.push_back(2); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
    return 2;
}

// POST-INCREMENT LOAD/STORE. `lb/lh/lw rd, 0(base); addi base,base,K` (or a store) → ONE uop that
// does the memory op at 0(base) and writes base+K — the trailing pointer bump folded in, ARM ldr/str
// post-increment style. Saves a uop per iteration in copy/scan/blit loops, which dominate blit-style hot
// paths (column blits, memcpy). Requires offset 0 (so w1 carries the increment), the addi to target the
// base reg, and for loads rd != base so the two writebacks are independent. Indirect jumps landing on
// the absorbed addi are covered by translate-on-miss (its pc stays unmapped → re-translated 1:1).
static int rvcud_try_postinc(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+1 >= N) return 0;
    uint32_t i0 = img[w], op = i0 & 0x7F, f3 = (i0>>12)&7;
    bool isld = (op==0x03 && (f3==0||f3==1||f3==2||f3==4||f3==5));
    bool isst = (op==0x23 && f3<=2);
    if (!isld && !isst) return 0;
    uint32_t imm = isld ? rv_iimm(i0) : rv_simm(i0);
    if (imm != 0) return 0;                                          // only 0(base) → w1 is free for the increment
    if (B.leader[w+1]) return 0;                                     // can't absorb a branch/foreign-entry target
    uint32_t i1 = img[w+1];
    if ((i1 & 0x7F) != 0x13 || ((i1>>12)&7) != 0) return 0;          // next instr must be addi
    uint32_t base = (i0>>15)&0x1F, ard = (i1>>7)&0x1F, ars1 = (i1>>15)&0x1F;
    if (base == 0 || ard != base || ars1 != base) return 0;          // addi base, base, K (post-increment the base)
    uint32_t rd = (i0>>7)&0x1F, rs2 = (i0>>20)&0x1F;
    if (isld && (rd == 0 || rd == base)) return 0;                   // load needs a real dest distinct from base
    int32_t K = (int32_t)rv_iimm(i1);
    B.pc2uop[w] = ui;
    if (isld) B.w0.push_back(RCW0(RC_LOADPI,  rd, base, 0,   f3, 0, RP_UNC, 0));
    else      B.w0.push_back(RCW0(RC_STOREPI, 0,  base, rs2, f3, 0, RP_UNC, 0));
    B.w1.push_back((uint32_t)K); B.uw.push_back(2);                  // retires 2 guest instrs (mem op + addi)
    B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
    return 2;
}

// SOFTWARE-MULTIPLY LOOP → MULLOOP. clang's rv32i shift-add software multiply (fixed-point multiply helpers) is a
// 7-instruction loop, structurally (regs generalized M=multiplier, B=multiplicand, A=temp, ACC=accum):
//   slli A,M,31; srli M,M,1; srai A,A,31; and A,A,B; add ACC,A,ACC; slli B,B,1; bne M,x0, →top
// computing ACC += B*M over the bits of M. Replaced by ONE uop that does a hardware multiply and sets
// the EXACT final state of all four registers (CPU-validated in test_mulloop.cpp, all regs, 800 cases),
// retiring weight = 7×iterations so RV32I-MIPS accounting stays exact. Matches by structure (any regs),
// so it fires on every call site (one shared __mulsi3 or inlined). Consumes 7 words.
static int rvcud_try_mulloop(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+7 >= N) return 0;                                          // 7 body words + a fall-through instr
    uint32_t i0=img[w], i1=img[w+1], i2=img[w+2], i3=img[w+3], i4=img[w+4], i5=img[w+5], i6=img[w+6];
    if ((i0&0x7F)!=0x13 || ((i0>>12)&7)!=1 || ((i0>>20)&0x1F)!=31) return 0;          // slli A,M,31
    uint32_t A=(i0>>7)&0x1F, M=(i0>>15)&0x1F;
    if ((i1&0x7F)!=0x13 || ((i1>>12)&7)!=5 || ((i1>>30)&1)!=0 || ((i1>>20)&0x1F)!=1
        || ((i1>>7)&0x1F)!=M || ((i1>>15)&0x1F)!=M) return 0;                         // srli M,M,1
    if ((i2&0x7F)!=0x13 || ((i2>>12)&7)!=5 || ((i2>>30)&1)!=1 || ((i2>>20)&0x1F)!=31
        || ((i2>>7)&0x1F)!=A || ((i2>>15)&0x1F)!=A) return 0;                         // srai A,A,31
    if ((i3&0x7F)!=0x33 || ((i3>>25)&0x7F)!=0 || ((i3>>12)&7)!=7 || ((i3>>7)&0x1F)!=A) return 0;  // and A,A,B
    uint32_t a3s1=(i3>>15)&0x1F, a3s2=(i3>>20)&0x1F, Bb;
    if      (a3s1==A) Bb=a3s2; else if (a3s2==A) Bb=a3s1; else return 0;
    if ((i4&0x7F)!=0x33 || ((i4>>25)&0x7F)!=0 || ((i4>>12)&7)!=0) return 0;           // add ACC,A,ACC
    uint32_t ACC=(i4>>7)&0x1F, a4s1=(i4>>15)&0x1F, a4s2=(i4>>20)&0x1F;
    if (!((a4s1==A && a4s2==ACC) || (a4s2==A && a4s1==ACC))) return 0;
    if ((i5&0x7F)!=0x13 || ((i5>>12)&7)!=1 || ((i5>>20)&0x1F)!=1
        || ((i5>>7)&0x1F)!=Bb || ((i5>>15)&0x1F)!=Bb) return 0;                       // slli B,B,1
    if ((i6&0x7F)!=0x63 || ((i6>>12)&7)!=1) return 0;                                 // bne M,x0,→top
    uint32_t b1=(i6>>15)&0x1F, b2=(i6>>20)&0x1F;
    if (!((b1==M && b2==0) || (b2==M && b1==0))) return 0;
    uint32_t bpc=B.base+(uint32_t)(w+6)*4, tw=(bpc + rv_bimm(i6) - B.base) >> 2;
    if ((int)tw != w) return 0;                                                       // back-edge to the loop header
    if (A==0||M==0||Bb==0||ACC==0 || A==M||A==Bb||A==ACC||M==Bb||M==ACC||Bb==ACC) return 0;  // 4 distinct, nonzero
    B.pc2uop[w] = ui;
    B.w0.push_back(RC_MULLOOP | (ACC<<7) | (M<<12) | (Bb<<17));      // pred bits[28:26]=0 ⇒ RP_UNC (not SETP)
    B.w1.push_back(A << 24);                                         // A in hi byte; low 24 = fall-through idx (Pass 3)
    B.uw.push_back(7); B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back((uint32_t)(w+7)); ui++;
    return 7;
}

// SOFTWARE-DIVIDE LOOP → DIVLOOP. clang's rv32i bit-serial restoring division (the body shared by
// __udivsi3 and __divsi3, and entered with R=0/Q=0/i=31/ONE=1/NEG1=-1) is a 13-instruction loop:
//   slli R,R,1; srl T1,N,i; sll T2,ONE,i; addi i,i,-1; andi T1,T1,1; or R,T1,R; sltu T1,R,D;
//   addi T1,T1,-1; and T2,T1,T2; and T1,T1,D; or Q,T2,Q; sub R,R,T1; bne i,NEG1,→top
// computing Q=N/D, R=N%D over 32 bits MSB-first. Replaced by ONE uop doing a hardware divide and the
// EXACT final state (Q=N/D, R=N%D, counter i→-1). The prologue inits (i=31,R=0,Q=0,ONE=1,NEG1=-1) are
// VERIFIED so the closed form is exact (CPU-validated, 8M cases, test_divloop.cpp). Matches by structure
// (any regs) so it fires on every call site of __udivsi3/__divsi3. Consumes 13 words. Generic — keys on
// the division idiom, not on any guest address. weight = 13×32 = 416 retired guest instrs (exact).
static int rvcud_try_divloop(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+13 >= N) return 0;                                          // 13 body words + a fall-through instr
    uint32_t i0=img[w],i1=img[w+1],i2=img[w+2],i3=img[w+3],i4=img[w+4],i5=img[w+5],
             i6=img[w+6],i7=img[w+7],i8=img[w+8],i9=img[w+9],i10=img[w+10],i11=img[w+11],i12=img[w+12];
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;};
    if ((i0&0x7F)!=0x13 || F3(i0)!=1 || S2(i0)!=1) return 0;          // slli R,R,1
    uint32_t R=RD(i0); if (S1(i0)!=R || R==0) return 0;
    if ((i1&0x7F)!=0x33 || F3(i1)!=5 || F7(i1)!=0) return 0;          // srl T1,N,i
    uint32_t T1=RD(i1), Nr=S1(i1), ir=S2(i1);
    if ((i2&0x7F)!=0x33 || F3(i2)!=1 || F7(i2)!=0 || S2(i2)!=ir) return 0;   // sll T2,ONE,i
    uint32_t T2=RD(i2), ONE=S1(i2);
    if ((i3&0x7F)!=0x13 || F3(i3)!=0 || RD(i3)!=ir || S1(i3)!=ir || (int32_t)rv_iimm(i3)!=-1) return 0;  // addi i,i,-1
    if ((i4&0x7F)!=0x13 || F3(i4)!=7 || RD(i4)!=T1 || S1(i4)!=T1 || rv_iimm(i4)!=1) return 0;            // andi T1,T1,1
    if ((i5&0x7F)!=0x33 || F3(i5)!=6 || F7(i5)!=0 || RD(i5)!=R) return 0;                                // or R,T1,R
    if (!((S1(i5)==T1&&S2(i5)==R)||(S1(i5)==R&&S2(i5)==T1))) return 0;
    if ((i6&0x7F)!=0x33 || F3(i6)!=3 || F7(i6)!=0 || RD(i6)!=T1 || S1(i6)!=R) return 0;                  // sltu T1,R,D
    uint32_t Dr=S2(i6);
    if ((i7&0x7F)!=0x13 || F3(i7)!=0 || RD(i7)!=T1 || S1(i7)!=T1 || (int32_t)rv_iimm(i7)!=-1) return 0;   // addi T1,T1,-1
    if ((i8&0x7F)!=0x33 || F3(i8)!=7 || F7(i8)!=0 || RD(i8)!=T2) return 0;                                // and T2,T1,T2
    if (!((S1(i8)==T1&&S2(i8)==T2)||(S1(i8)==T2&&S2(i8)==T1))) return 0;
    if ((i9&0x7F)!=0x33 || F3(i9)!=7 || F7(i9)!=0 || RD(i9)!=T1) return 0;                                // and T1,T1,D
    if (!((S1(i9)==T1&&S2(i9)==Dr)||(S1(i9)==Dr&&S2(i9)==T1))) return 0;
    if ((i10&0x7F)!=0x33 || F3(i10)!=6 || F7(i10)!=0) return 0;                                           // or Q,T2,Q
    uint32_t Q=RD(i10);
    if (!((S1(i10)==T2&&S2(i10)==Q)||(S1(i10)==Q&&S2(i10)==T2))) return 0;
    if ((i11&0x7F)!=0x33 || F3(i11)!=0 || F7(i11)!=0x20 || RD(i11)!=R || S1(i11)!=R || S2(i11)!=T1) return 0;  // sub R,R,T1
    if ((i12&0x7F)!=0x63 || F3(i12)!=1) return 0;                                                         // bne i,NEG1,→top
    uint32_t bb1=S1(i12), bb2=S2(i12), NEG1;
    if (bb1==ir) NEG1=bb2; else if (bb2==ir) NEG1=bb1; else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+12)*4, tw=(bpc+rv_bimm(i12)-B.base)>>2;
    if ((int)tw != w) return 0;                                      // back-edge to the loop header
    // role registers nonzero & the data regs distinct (consts ONE/NEG1 may not clash with data)
    if (R==0||Nr==0||Dr==0||ir==0||Q==0||T1==0||T2==0||ONE==0||NEG1==0) return 0;
    if (R==Nr||R==Dr||R==Q||R==ir||Nr==Dr||Q==ir||T1==T2||R==T1||R==T2) return 0;
    // VERIFY the prologue establishes the standard entry (so the closed form is exact). Scan back within
    // the block for li i,31 / li R,0 / li Q,0 / li ONE,1 / li NEG1,-1 (addi rd,x0,imm).
    bool gI=false,gR=false,gQ=false,gONE=false,gNEG=false;
    for (int j=w-1; j>=0 && j>=w-24; j--) {
        uint32_t in=img[j];
        bool li0 = ((in&0x7F)==0x13 && F3(in)==0 && S1(in)==0);      // addi rd,x0,imm == li rd,imm
        if (li0) { uint32_t rd=RD(in); int32_t k=(int32_t)rv_iimm(in);
            if (rd==ir   && k==31) gI=true;
            if (rd==R    && k==0)  gR=true;
            if (rd==Q    && k==0)  gQ=true;
            if (rd==ONE  && k==1)  gONE=true;
            if (rd==NEG1 && k==-1) gNEG=true; }
        if (B.leader[j]) break;                                      // don't cross into another block
    }
    if (!(gI&&gR&&gQ&&gONE&&gNEG)) return 0;
    B.pc2uop[w] = ui;
    B.w0.push_back(RC_DIVLOOP | (Q<<7) | (R<<12) | (Nr<<17));         // rd=Q, rs1=R, rs2=N (pred bits 0 ⇒ RP_UNC)
    B.w1.push_back((Dr<<22) | (ir<<27));                             // low 22 = fall-through (Pass 3); D[26:22]; i[31:27]
    B.uw.push_back(13); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+13)); ui++;
    return 13;
}

// PALETTE-EXPAND LOOP → PALEXP. The 8bpp→32bpp present/blit idiom clang emits as a 14-instruction loop:
//   lbu IDX,0(SRC); addi SRC,SRC,1; slli T,IDX,1; add P,PAL,IDX; add P,P,T; lbu C,0(P); sb C,-3(DST);
//   lbu C,1(P); sb C,-2(DST); lbu C,2(P); sb C,-1(DST); sb ALPHA,0(DST); addi DST,DST,4; bne SRC,END,top
// i.e. for each source palette byte: P = PAL + 3*IDX; write {pal[P],pal[P+1],pal[P+2],ALPHA} as RGBA to
// DST; SRC++; DST+=4. Replaced by ONE native whole-loop uop. PAL/ALPHA/END are loop-invariant. Matched by
// structure (any regs) → generic (no guest addresses). Consumes 14 words. weight = 14 retired/iteration.
static int rvcud_try_palexp(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+14 >= N) return 0;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;};
    uint32_t i0=img[w],i1=img[w+1],i2=img[w+2],i3=img[w+3],i4=img[w+4],i5=img[w+5],i6=img[w+6],
             i7=img[w+7],i8=img[w+8],i9=img[w+9],i10=img[w+10],i11=img[w+11],i12=img[w+12],i13=img[w+13];
    // w+0: lbu IDX,0(SRC)
    if ((i0&0x7F)!=0x03 || F3(i0)!=4 || rv_iimm(i0)!=0) return 0;
    uint32_t IDX=RD(i0), SRC=S1(i0);
    // w+1: addi SRC,SRC,1
    if ((i1&0x7F)!=0x13 || F3(i1)!=0 || RD(i1)!=SRC || S1(i1)!=SRC || rv_iimm(i1)!=1) return 0;
    // w+2: slli T,IDX,1
    if ((i2&0x7F)!=0x13 || F3(i2)!=1 || S1(i2)!=IDX || S2(i2)!=1) return 0;
    uint32_t T=RD(i2);
    // w+3: add P,PAL,IDX  (one operand is IDX)
    if ((i3&0x7F)!=0x33 || F3(i3)!=0 || F7(i3)!=0) return 0;
    uint32_t P=RD(i3), PAL;
    if (S1(i3)==IDX) PAL=S2(i3); else if (S2(i3)==IDX) PAL=S1(i3); else return 0;
    // w+4: add P,P,T
    if ((i4&0x7F)!=0x33 || F3(i4)!=0 || F7(i4)!=0 || RD(i4)!=P) return 0;
    if (!((S1(i4)==P&&S2(i4)==T)||(S1(i4)==T&&S2(i4)==P))) return 0;
    // w+5: lbu C0,0(P)
    if ((i5&0x7F)!=0x03 || F3(i5)!=4 || S1(i5)!=P || rv_iimm(i5)!=0) return 0;
    uint32_t C0=RD(i5);
    // w+6: sb C0,-3(DST)
    if ((i6&0x7F)!=0x23 || F3(i6)!=0 || S2(i6)!=C0 || (int32_t)rv_simm(i6)!=-3) return 0;
    uint32_t DST=S1(i6);
    // w+7: lbu C1,1(P)
    if ((i7&0x7F)!=0x03 || F3(i7)!=4 || S1(i7)!=P || rv_iimm(i7)!=1) return 0;
    uint32_t C1=RD(i7);
    // w+8: sb C1,-2(DST)
    if ((i8&0x7F)!=0x23 || F3(i8)!=0 || S1(i8)!=DST || S2(i8)!=C1 || (int32_t)rv_simm(i8)!=-2) return 0;
    // w+9: lbu C2,2(P)
    if ((i9&0x7F)!=0x03 || F3(i9)!=4 || S1(i9)!=P || rv_iimm(i9)!=2) return 0;
    uint32_t C2=RD(i9);
    // w+10: sb C2,-1(DST)
    if ((i10&0x7F)!=0x23 || F3(i10)!=0 || S1(i10)!=DST || S2(i10)!=C2 || (int32_t)rv_simm(i10)!=-1) return 0;
    // w+11: sb ALPHA,0(DST)
    if ((i11&0x7F)!=0x23 || F3(i11)!=0 || S1(i11)!=DST || rv_simm(i11)!=0) return 0;
    uint32_t ALPHA=S2(i11);
    // w+12: addi DST,DST,4
    if ((i12&0x7F)!=0x13 || F3(i12)!=0 || RD(i12)!=DST || S1(i12)!=DST || rv_iimm(i12)!=4) return 0;
    // w+13: bne SRC,END,→w
    if ((i13&0x7F)!=0x63 || F3(i13)!=1) return 0;
    uint32_t END;
    if (S1(i13)==SRC) END=S2(i13); else if (S2(i13)==SRC) END=S1(i13); else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+13)*4, tw=(bpc+rv_bimm(i13)-B.base)>>2;
    if ((int)tw != w) return 0;                                      // back-edge to the loop header
    // the five live registers must be distinct & nonzero (SRC,DST,PAL,END,ALPHA); PAL,END,ALPHA invariant
    if (SRC==0||DST==0||PAL==0||END==0||ALPHA==0) return 0;
    if (SRC==DST||SRC==PAL||SRC==END||DST==PAL||DST==END||DST==ALPHA||SRC==ALPHA) return 0;
    if (PAL==SRC||PAL==DST) return 0;
    B.pc2uop[w] = ui;
    B.w0.push_back(RC_PALEXP | (SRC<<7) | (DST<<12) | (PAL<<17));     // rd=SRC, rs1=DST, rs2=PAL (pred bits 0)
    B.w1.push_back((END<<22) | (ALPHA<<27));                         // low 22 = fall-through (Pass 3); END[26:22]; ALPHA[31:27]
    B.uw.push_back(14); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+14)); ui++;
    return 14;
}

// TEXTURE-MAPPED SPAN LOOP → TEXSPAN. The texture-mapped span idiom (19 instructions):
//   srli Yt,YPOS,shY; slli Xt,XPOS,shX1; lw CMAP,oC(BASE); lw TEX,oT(BASE); and Yt,Yt,MASK;
//   srli Xt,Xt,shX2; add OFF,Yt,Xt; add TA,TEX,OFF; lbu PIDX,0(TA); add CA,CMAP,PIDX; lbu PIX,0(CA);
//   sb PIX,0(DST); lw XS,oX(BASE); lw YS,oY(BASE); addi nD,DST,1; add XPOS,XS,XPOS; add YPOS,YS,YPOS;
//   mv DST,nD; bne nD,END,top
// i.e. pix = cmap[ tex[ ((YPOS>>shY)&MASK) + ((XPOS<<shX1)>>shX2) ] ]; *DST++ = pix; XPOS+=xstep; YPOS+=ystep.
// → ONE native whole-loop uop. Shifts/offsets/regs are READ from the matched instrs (not hardcoded → generic)
// and stored in g_ext (too many for a uint2). Re-loads the 4 invariants each iter in guest order (alias-safe →
// bit-identical). Consumes 19 words; weight = 19 retired/iteration.
static int rvcud_try_texspan(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+19 >= N) return 0;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;}; auto SH=[](uint32_t x){return (x>>20)&0x1F;};
    uint32_t a[19]; for (int k=0;k<19;k++) a[k]=img[w+k];
    // w+0 srli Yt,YPOS,shY ; w+1 slli Xt,XPOS,shX1
    if ((a[0]&0x7F)!=0x13||F3(a[0])!=5||F7(a[0])!=0) return 0;  uint32_t Yt=RD(a[0]),YPOS=S1(a[0]),shY=SH(a[0]);
    if ((a[1]&0x7F)!=0x13||F3(a[1])!=1) return 0;               uint32_t Xt=RD(a[1]),XPOS=S1(a[1]),shX1=SH(a[1]);
    // w+2 lw CMAP,oC(BASE) ; w+3 lw TEX,oT(BASE)
    if ((a[2]&0x7F)!=0x03||F3(a[2])!=2) return 0;  uint32_t CMAP=RD(a[2]),BASE=S1(a[2]),oC=rv_iimm(a[2]);
    if ((a[3]&0x7F)!=0x03||F3(a[3])!=2||S1(a[3])!=BASE) return 0;  uint32_t TEX=RD(a[3]),oT=rv_iimm(a[3]);
    // w+4 and Yt,Yt,MASK
    if ((a[4]&0x7F)!=0x33||F3(a[4])!=7||F7(a[4])!=0||RD(a[4])!=Yt) return 0;
    uint32_t MASK; if (S1(a[4])==Yt) MASK=S2(a[4]); else if (S2(a[4])==Yt) MASK=S1(a[4]); else return 0;
    // w+5 srli Xt,Xt,shX2
    if ((a[5]&0x7F)!=0x13||F3(a[5])!=5||F7(a[5])!=0||RD(a[5])!=Xt||S1(a[5])!=Xt) return 0;  uint32_t shX2=SH(a[5]);
    // w+6 add OFF,Yt,Xt
    if ((a[6]&0x7F)!=0x33||F3(a[6])!=0||F7(a[6])!=0) return 0;  uint32_t OFF=RD(a[6]);
    if (!((S1(a[6])==Yt&&S2(a[6])==Xt)||(S1(a[6])==Xt&&S2(a[6])==Yt))) return 0;
    // w+7 add TA,TEX,OFF
    if ((a[7]&0x7F)!=0x33||F3(a[7])!=0||F7(a[7])!=0) return 0;  uint32_t TA=RD(a[7]);
    if (!((S1(a[7])==TEX&&S2(a[7])==OFF)||(S1(a[7])==OFF&&S2(a[7])==TEX))) return 0;
    // w+8 lbu PIDX,0(TA)
    if ((a[8]&0x7F)!=0x03||F3(a[8])!=4||S1(a[8])!=TA||rv_iimm(a[8])!=0) return 0;  uint32_t PIDX=RD(a[8]);
    // w+9 add CA,CMAP,PIDX
    if ((a[9]&0x7F)!=0x33||F3(a[9])!=0||F7(a[9])!=0) return 0;  uint32_t CA=RD(a[9]);
    if (!((S1(a[9])==CMAP&&S2(a[9])==PIDX)||(S1(a[9])==PIDX&&S2(a[9])==CMAP))) return 0;
    // w+10 lbu PIX,0(CA)
    if ((a[10]&0x7F)!=0x03||F3(a[10])!=4||S1(a[10])!=CA||rv_iimm(a[10])!=0) return 0;  uint32_t PIX=RD(a[10]);
    // w+11 sb PIX,0(DST)
    if ((a[11]&0x7F)!=0x23||F3(a[11])!=0||S2(a[11])!=PIX||rv_simm(a[11])!=0) return 0;  uint32_t DST=S1(a[11]);
    // w+12 lw XS,oX(BASE) ; w+13 lw YS,oY(BASE)
    if ((a[12]&0x7F)!=0x03||F3(a[12])!=2||S1(a[12])!=BASE) return 0;  uint32_t XS=RD(a[12]),oX=rv_iimm(a[12]);
    if ((a[13]&0x7F)!=0x03||F3(a[13])!=2||S1(a[13])!=BASE) return 0;  uint32_t YS=RD(a[13]),oY=rv_iimm(a[13]);
    // w+14 addi nD,DST,1
    if ((a[14]&0x7F)!=0x13||F3(a[14])!=0||S1(a[14])!=DST||rv_iimm(a[14])!=1) return 0;  uint32_t nD=RD(a[14]);
    // w+15 add XPOS,XS,XPOS ; w+16 add YPOS,YS,YPOS
    if ((a[15]&0x7F)!=0x33||F3(a[15])!=0||F7(a[15])!=0||RD(a[15])!=XPOS) return 0;
    if (!((S1(a[15])==XS&&S2(a[15])==XPOS)||(S1(a[15])==XPOS&&S2(a[15])==XS))) return 0;
    if ((a[16]&0x7F)!=0x33||F3(a[16])!=0||F7(a[16])!=0||RD(a[16])!=YPOS) return 0;
    if (!((S1(a[16])==YS&&S2(a[16])==YPOS)||(S1(a[16])==YPOS&&S2(a[16])==YS))) return 0;
    // w+17 mv DST,nD  (addi DST,nD,0) ; w+18 bne nD,END,top
    if ((a[17]&0x7F)!=0x13||F3(a[17])!=0||RD(a[17])!=DST||S1(a[17])!=nD||rv_iimm(a[17])!=0) return 0;
    if ((a[18]&0x7F)!=0x63||F3(a[18])!=1) return 0;
    uint32_t END; if (S1(a[18])==nD) END=S2(a[18]); else if (S2(a[18])==nD) END=S1(a[18]); else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+18)*4, tw=(bpc+rv_bimm(a[18])-B.base)>>2;
    if ((int)tw != w) return 0;                                      // back-edge to the loop header
    // the live registers (XPOS,YPOS,DST,END,MASK,BASE) must be distinct & nonzero
    uint32_t lv[6]={XPOS,YPOS,DST,END,MASK,BASE};
    for (int p=0;p<6;p++){ if(lv[p]==0) return 0; for(int q=p+1;q<6;q++) if(lv[p]==lv[q]) return 0; }
    if (oC>0xFFF||oT>0xFFF||oX>0xFFF||oY>0xFFF) return 0;            // offsets must fit the ext packing
    uint32_t e = (uint32_t)B.ext.size();
    B.ext.push_back((XPOS&31)|((YPOS&31)<<5)|((DST&31)<<10)|((END&31)<<15)|((MASK&31)<<20)|((BASE&31)<<25));
    B.ext.push_back((oC&0xFFF)|((oT&0xFFF)<<12));
    B.ext.push_back((oX&0xFFF)|((oY&0xFFF)<<12));
    B.ext.push_back((shY&31)|((shX1&31)<<5)|((shX2&31)<<10));
    B.pc2uop[w] = ui;
    B.w0.push_back(RC_TEXSPAN | (e<<7));                             // ext base index in bits[31:7]
    B.w1.push_back(0);                                              // fall-through baked in Pass 3 (full w1)
    B.uw.push_back(19); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+19)); ui++;
    return 19;
}

// TEXTURED-COLUMN LOOP → TEXCOL. The textured-column idiom (12 instrs):
//   lw TEX,oT(BASE); lw CMAP,oC(BASE); slli C,FRAC,sa; srli C,C,sb; add TA,TEX,C; lbu PIDX,0(TA);
//   add CA,CMAP,PIDX; lbu PIX,0(CA); sb PIX,0(DST); addi DST,DST,STR; add FRAC,FRAC,STEP; bne DST,END,top
// = vertical 1D textured column: pix=cmap[tex[(frac<<sa)>>sb]]; *dst=pix; dst+=STR; frac+=STEP. Params
// (regs/offsets/shifts/stride read from the matched instrs, never hardcoded) live in g_ext. Consumes 12.
static int rvcud_try_texcol(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    if (w+12 >= N) return 0;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;}; auto SH=[](uint32_t x){return (x>>20)&0x1F;};
    uint32_t a[12]; for (int k=0;k<12;k++) a[k]=img[w+k];
    if ((a[0]&0x7F)!=0x03||F3(a[0])!=2) return 0;  uint32_t TEX=RD(a[0]),BASE=S1(a[0]),oT=rv_iimm(a[0]);   // lw TEX,oT(BASE)
    if ((a[1]&0x7F)!=0x03||F3(a[1])!=2||S1(a[1])!=BASE) return 0;  uint32_t CMAP=RD(a[1]),oC=rv_iimm(a[1]);// lw CMAP,oC(BASE)
    if ((a[2]&0x7F)!=0x13||F3(a[2])!=1) return 0;  uint32_t C=RD(a[2]),FRAC=S1(a[2]),sa=SH(a[2]);          // slli C,FRAC,sa
    if ((a[3]&0x7F)!=0x13||F3(a[3])!=5||F7(a[3])!=0||RD(a[3])!=C||S1(a[3])!=C) return 0;  uint32_t sb=SH(a[3]);// srli C,C,sb
    if ((a[4]&0x7F)!=0x33||F3(a[4])!=0||F7(a[4])!=0) return 0;  uint32_t TA=RD(a[4]);                      // add TA,TEX,C
    if (!((S1(a[4])==TEX&&S2(a[4])==C)||(S1(a[4])==C&&S2(a[4])==TEX))) return 0;
    if ((a[5]&0x7F)!=0x03||F3(a[5])!=4||S1(a[5])!=TA||rv_iimm(a[5])!=0) return 0;  uint32_t PIDX=RD(a[5]); // lbu PIDX,0(TA)
    if ((a[6]&0x7F)!=0x33||F3(a[6])!=0||F7(a[6])!=0) return 0;  uint32_t CA=RD(a[6]);                      // add CA,CMAP,PIDX
    if (!((S1(a[6])==CMAP&&S2(a[6])==PIDX)||(S1(a[6])==PIDX&&S2(a[6])==CMAP))) return 0;
    if ((a[7]&0x7F)!=0x03||F3(a[7])!=4||S1(a[7])!=CA||rv_iimm(a[7])!=0) return 0;  uint32_t PIX=RD(a[7]);  // lbu PIX,0(CA)
    if ((a[8]&0x7F)!=0x23||F3(a[8])!=0||S2(a[8])!=PIX||rv_simm(a[8])!=0) return 0;  uint32_t DST=S1(a[8]); // sb PIX,0(DST)
    if ((a[9]&0x7F)!=0x13||F3(a[9])!=0||RD(a[9])!=DST||S1(a[9])!=DST) return 0;  int32_t STR=(int32_t)rv_iimm(a[9]); // addi DST,DST,STR
    if ((a[10]&0x7F)!=0x33||F3(a[10])!=0||F7(a[10])!=0||RD(a[10])!=FRAC) return 0;                        // add FRAC,FRAC,STEP
    uint32_t STEP; if (S1(a[10])==FRAC) STEP=S2(a[10]); else if (S2(a[10])==FRAC) STEP=S1(a[10]); else return 0;
    if ((a[11]&0x7F)!=0x63||F3(a[11])!=1) return 0;                                                       // bne DST,END,top
    uint32_t END; if (S1(a[11])==DST) END=S2(a[11]); else if (S2(a[11])==DST) END=S1(a[11]); else return 0;
    uint32_t bpc=B.base+(uint32_t)(w+11)*4, tw=(bpc+rv_bimm(a[11])-B.base)>>2;
    if ((int)tw != w) return 0;                                                                          // back-edge to header
    uint32_t lv[5]={FRAC,DST,END,BASE,STEP};
    for (int p=0;p<5;p++){ if(lv[p]==0) return 0; for(int q=p+1;q<5;q++) if(lv[p]==lv[q]) return 0; }
    if (oT>0xFFF || oC>0xFFF || STR<-32768 || STR>32767) return 0;
    uint32_t e=(uint32_t)B.ext.size();
    B.ext.push_back((FRAC&31)|((DST&31)<<5)|((END&31)<<10)|((BASE&31)<<15)|((STEP&31)<<20));
    B.ext.push_back((oT&0xFFF)|((oC&0xFFF)<<12));
    B.ext.push_back((sa&31)|((sb&31)<<5)|(((uint32_t)STR&0xFFFF)<<10));
    B.pc2uop[w]=ui;
    B.w0.push_back(RC_TEXCOL | (e<<7));
    B.w1.push_back(0);                                              // fall-through baked Pass 3 (full w1)
    B.uw.push_back(12); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)(w+12)); ui++;
    return 12;
}

static void rvcud_build(RvcudBuild& B) {
    int N = B.nwords;
    // Pass 1 — leaders (any uop start) and xtargets (explicit foreign entries only).
    B.leader.assign(N, 0); B.xtarget.assign(N, 0);
    if (B.entry >= B.base) { uint32_t ew = (B.entry - B.base) >> 2; if ((int)ew < N) { B.leader[ew]=1; B.xtarget[ew]=1; } }
    for (int w = 0; w < N; w++) {
        uint32_t instr = B.img[w], op = instr & 0x7F, pcw = B.base + (uint32_t)w*4;
        if (op == 0x63 || op == 0x6F) {
            uint32_t d = (op==0x63) ? rv_bimm(instr) : rv_jimm(instr);
            uint32_t tw = (pcw + d - B.base) >> 2;
            if ((int)tw < N) { B.leader[tw] = 1; B.xtarget[tw] = 1; }   // explicit target = foreign entry
            if (w+1 < N) B.leader[w+1] = 1;                            // fall-through (not a foreign entry)
        } else if (op == 0x67) { if (w+1 < N) B.leader[w+1] = 1; }
    }
    rvcud_liveness(B);
    // Pass 2 — emit uops (fusion may consume >1 word, never crossing/swallowing a leader).
    // RVCUD_FUSEMASK (hex bitmask, default all-on) gates each matcher — debug/bisect tool.
    unsigned fm = 0xFFFFFFFFu;
    { const char* s = getenv("RVCUD_FUSEMASK"); if (s) fm = (unsigned)strtoul(s, nullptr, 16); }
    auto FM = [&](int i){ return (fm >> i) & 1u; };
    B.pc2uop.assign(N, RC_BADUOP);
    uint32_t ui = 0;
    for (int w = 0; w < N; ) {
        int consumed = FM(0) ? rvcud_try_mulloop(B, w, ui) : 0;    // shift-add software-multiply loop → 1 hw multiply
        if (consumed == 0 && FM(1)) consumed = rvcud_try_divloop(B, w, ui);  // bit-serial software-divide loop → 1 hw divide
        if (consumed == 0 && FM(2)) consumed = rvcud_try_palexp(B, w, ui);   // 8bpp→32bpp palette-expand loop → 1 native loop uop
        if (consumed == 0 && FM(3)) consumed = rvcud_try_texspan(B, w, ui);  // texture-mapped span loop → 1 native loop uop
        if (consumed == 0 && FM(4)) consumed = rvcud_try_texcol(B, w, ui);   // textured-column loop → 1 native loop uop
        if (consumed == 0 && FM(5)) consumed = rvcud_try_incbr(B, w, ui);   // counted-loop addi+branch → INCBR
        if (consumed == 0 && FM(6)) consumed = rvcud_try_ifconv(B, w, ui);  // if-conversion (short forward branch → predicated)
        if (consumed == 0 && FM(7)) consumed = rvcud_try_affine(B, w, ui);  // affine ×const fold → MULADD/MULC + ADD chain
        if (consumed == 0 && FM(8)) consumed = rvcud_try_memset(B, w, ui);    // whole byte-fill (memset) loop → 1 native uop
        if (consumed == 0 && FM(9)) consumed = rvcud_try_wordfill(B, w, ui);  // direct-form sX-fill loop → 1 native uop
        if (consumed == 0 && FM(10)) consumed = rvcud_try_wordscan(B, w, ui);  // counted search loop → 1 native uop
        if (consumed == 0 && FM(11)) consumed = rvcud_try_copyloopt(B, w, ui);   // temp-advance strided copy loop → 1 native uop
        if (consumed == 0 && FM(12)) consumed = rvcud_try_copyloop(B, w, ui);  // whole byte-memcpy loop → 1 word-widened uop
        if (consumed == 0 && FM(13)) consumed = rvcud_try_copyloops(B, w, ui); // whole strided copy loop → 1 native uop
        if (consumed == 0 && FM(14)) consumed = rvcud_try_copy(B, w, ui);     // lb;sb;addi;addi memory-copy step → 1 uop
        if (consumed == 0 && FM(15)) consumed = rvcud_try_ldxs(B, w, ui);     // slli + add + load → scaled-indexed load (1 uop)
        if (consumed == 0 && FM(16)) consumed = rvcud_try_ldx(B, w, ui);      // add + load/store → indexed mem op (1 uop)
        if (consumed == 0 && FM(17)) consumed = rvcud_try_postinc(B, w, ui);  // load/store + base post-increment → 1 uop
        if (consumed == 0) {
            B.pc2uop[w] = ui;
            uint32_t a0, a1, t; uint8_t wt;
            if (FM(18)) consumed = rvcud_try_fuse(B, w, a0, a1, wt, t);   // ≥1; swallowed words stay BADUOP (LEA/CONST/XSH)
            else        consumed = rvcud_classify(B, w, a0, a1, wt, t);   // bisect mode: pure 1:1
            B.w0.push_back(a0); B.w1.push_back(a1); B.uw.push_back(wt);
            B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(t);
            ui++;
        }
        w += consumed;
    }
    // Pass 3 — bake direct branch/JAL targets to uop indices (halt sentinel if target isn't a leader).
    for (size_t i = 0; i < B.w0.size(); i++) {
        uint32_t cls = B.w0[i] & 0x7F;
        if (cls == RC_BR || cls == RC_JAL) {
            uint32_t tw = B.tgt[i];
            B.w1[i] = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? B.pc2uop[tw] : 0xFFFFFFFFu;
        }
        else if (cls == RC_INCBR) {                                  // preserve K (high 8 bits); bake target into low 24
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x00FFFFFFu) : 0x00FFFFFFu;
            B.w1[i] = (B.w1[i] & 0xFF000000u) | idx;
        }
        else if (cls == RC_COPYLOOP || cls == RC_MULLOOP || cls == RC_MEMSET || cls == RC_WORDFILL) {   // preserve hi byte (reg/K); bake fall-through into low 24
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x00FFFFFFu) : 0x00FFFFFFu;
            B.w1[i] = (B.w1[i] & 0xFF000000u) | idx;
        }
        else if (cls == RC_DIVLOOP || cls == RC_PALEXP) {            // preserve bits[31:22] (regs); bake fall-through into low 22
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x003FFFFFu) : 0x003FFFFFu;
            B.w1[i] = (B.w1[i] & 0xFFC00000u) | idx;
        }
        else if (cls == RC_TEXSPAN || cls == RC_COPYLOOPS || cls == RC_TEXCOL || cls == RC_COPYLOOPT) {  // w1 = full fall-through uop index (params in ext[])
            uint32_t tw = B.tgt[i];
            B.w1[i] = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? B.pc2uop[tw] : 0xFFFFFFFFu;
        }
        else if (cls == RC_WORDSCAN) {                               // w1 = fall-through; ext[e+2] word → exit-target uop idx
            uint32_t tw = B.tgt[i];
            B.w1[i] = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? B.pc2uop[tw] : 0xFFFFFFFFu;
            uint32_t e = B.w0[i] >> 7, xw = B.ext[e+2];
            B.ext[e+2] = ((int)xw < N && B.pc2uop[xw] != RC_BADUOP) ? B.pc2uop[xw] : 0xFFFFFFFFu;
        }
    }
}

static void rvcud_free() {
    if (g_uops)   { cudaFree(g_uops);   g_uops   = nullptr; }
    if (g_uw)     { cudaFree(g_uw);     g_uw     = nullptr; }
    if (g_pc2uop) { cudaFree(g_pc2uop); g_pc2uop = nullptr; }
    if (g_uop2pc) { cudaFree(g_uop2pc); g_uop2pc = nullptr; }
    if (g_ext)    { cudaFree(g_ext);    g_ext    = nullptr; }
    g_nuops = 0; g_uopcap = 0;
    g_img.clear(); g_pc2uop_h.clear(); g_w0h.clear(); g_w1h.clear(); g_u2pch.clear(); g_uwh.clear();
}

// Translate the guest image into the rvcud uop stream and upload it. `base` is the
// guest image base (0x1000); `entry` the guest entry pc (so it stays a uop leader).
API int cuda_rvcud_set_code(const void* src, unsigned int len, unsigned int base, unsigned int entry) {
    rvcud_free();
    int N = (int)(len >> 2);
    g_img.assign(N, 0);
    memcpy(g_img.data(), src, (size_t)N * 4);
    RvcudBuild B; B.nwords = N; B.base = base; B.entry = entry; B.img = g_img.data();
    rvcud_build(B);
    g_nuops = (int)B.w0.size(); g_base = base; g_pc2words = N;
    // Persist host mirrors so missed (indirect) targets can be translated lazily and appended.
    g_w0h = B.w0; g_w1h = B.w1; g_uwh = B.uw; g_u2pch = B.u2pc; g_pc2uop_h = B.pc2uop;
    // Device capacity has headroom for translate-on-miss: each code word can be re-emitted once as a
    // 1:1 uop, plus up to one rejoin-JAL per block → ≤ 2N appended.
    g_uopcap = g_nuops + 2 * N + 16;
    std::vector<uint2> uops(g_nuops);
    for (int i = 0; i < g_nuops; i++) uops[i] = make_uint2(g_w0h[i], g_w1h[i]);

    cudaError_t e;
    if ((e = cudaMalloc(&g_uops,   (size_t)g_uopcap * sizeof(uint2)))  != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_uw,     (size_t)g_uopcap))                  != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_uop2pc, (size_t)g_uopcap * 4))             != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_pc2uop, (size_t)N * 4))                    != cudaSuccess) return (int)e;
    size_t next = B.ext.empty() ? 1 : B.ext.size();                   // ≥1 so the kernel always has a valid ptr
    if ((e = cudaMalloc(&g_ext, next * 4)) != cudaSuccess) return (int)e;
    if (!B.ext.empty()) cudaMemcpy(g_ext, B.ext.data(), B.ext.size() * 4, cudaMemcpyHostToDevice);
    cudaMemcpy(g_uops,   uops.data(),     (size_t)g_nuops * sizeof(uint2), cudaMemcpyHostToDevice);
    cudaMemcpy(g_uw,     g_uwh.data(),    (size_t)g_nuops,                cudaMemcpyHostToDevice);
    cudaMemcpy(g_uop2pc, g_u2pch.data(),  (size_t)g_nuops * 4,            cudaMemcpyHostToDevice);
    cudaMemcpy(g_pc2uop, g_pc2uop_h.data(), (size_t)N * 4,               cudaMemcpyHostToDevice);
    int rc = (int)cudaDeviceSynchronize();
    rvxblk_build();                          // cross-compile the statically-reached hot words to an exec_block PTX kernel
    return rc;
}

// Translate-on-miss: an indirect (JALR) jump landed on code with no uop. Lazily translate from `pc`
// as a straight-line 1:1 run — continuing through conditional branches (their fall-through is the
// next appended uop) and stopping at an unconditional jal/jalr, code end, or where it rejoins an
// already-translated word (emit a jump to it). Direct targets are baked against the live pc2uop.
// Appends to the (capacity-reserved) device buffers and patches the changed pc2uop range.
static bool rvcud_translate_miss(uint32_t pc) {
    if (pc < g_base) return false;
    int w = (int)((pc - g_base) >> 2), N = g_pc2words;
    if (w < 0 || w >= N || g_pc2uop_h[w] != RC_BADUOP) return false;
    RvcudBuild tb; tb.img = g_img.data(); tb.nwords = N; tb.base = g_base;
    int firstNew = g_nuops, j = w;
    for (; j < N && g_nuops < g_uopcap - 1; j++) {
        if (j != w && g_pc2uop_h[j] != RC_BADUOP) {        // rejoin existing translation
            g_w0h.push_back(RCW0(RC_JAL,0,0,0,0,0,RP_UNC,0));
            g_w1h.push_back(g_pc2uop_h[j]); g_uwh.push_back(0); g_u2pch.push_back(g_base+(uint32_t)j*4);
            g_nuops++; break;
        }
        if (g_x_sptrust && !rvx_sp_writer_ok(g_img[j])) {   // newly-reached code breaks the sp proof
            g_xblk_ok = 0; g_x_sptrust = 0;
            fprintf(stderr,"[xblk] exec disabled: translate-on-miss found an unprovable sp writer at 0x%08X\n",
                    g_base + (uint32_t)j*4);
        }
        uint32_t a0,a1,t; uint8_t wt; rvcud_classify(tb, j, a0,a1,wt,t);
        g_pc2uop_h[j] = (uint32_t)g_nuops;
        g_w0h.push_back(a0); g_w1h.push_back(a1); g_uwh.push_back(wt); g_u2pch.push_back(g_base+(uint32_t)j*4);
        uint32_t cls = a0 & 0x7F;
        if (cls == RC_BR || cls == RC_JAL)                 // bake direct target against the live map
            g_w1h[g_nuops] = ((int)t < N && g_pc2uop_h[t] != RC_BADUOP) ? g_pc2uop_h[t] : 0xFFFFFFFFu;
        g_nuops++;
        uint32_t op = g_img[j] & 0x7F;
        if (op == 0x6F || op == 0x67) { j++; break; }      // jal/jalr: unconditional → block ends
    }
    int cnt = g_nuops - firstNew;
    if (cnt <= 0) return false;
    std::vector<uint2> nu(cnt);
    for (int i = 0; i < cnt; i++) nu[i] = make_uint2(g_w0h[firstNew+i], g_w1h[firstNew+i]);
    cudaMemcpy(g_uops   + firstNew, nu.data(),            (size_t)cnt * sizeof(uint2), cudaMemcpyHostToDevice);
    cudaMemcpy(g_uw     + firstNew, g_uwh.data()+firstNew,(size_t)cnt,                 cudaMemcpyHostToDevice);
    cudaMemcpy(g_uop2pc + firstNew, g_u2pch.data()+firstNew,(size_t)cnt * 4,           cudaMemcpyHostToDevice);
    cudaMemcpy(g_pc2uop + w, g_pc2uop_h.data()+w, (size_t)(j - w) * 4, cudaMemcpyHostToDevice);
    cudaDeviceSynchronize();
    return true;
}

// Budget is a GUEST-INSTRUCTION budget (the kernel accumulates per-uop weights), so the
// existing cores*budget*iters MIPS metric stays guest-MIPS and is apples-to-apples with rv32i.
API int cuda_rvcud_step_all(int budget) {
    if (g_ncores <= 0 || g_nuops <= 0) return -1;
    if (!g_ret) { cudaHostAlloc((void**)&g_ret, 16, cudaHostAllocMapped); memset((void*)g_ret, 0, 16); }
#if RVCUD_HOTHIST
    { static unsigned long long* hot = nullptr;
      if (!hot) { cudaMallocManaged(&hot, (size_t)g_uopcap * 8); cudaMemset(hot, 0, (size_t)g_uopcap * 8);
                  cudaMemcpyToSymbol(g_dev_hot, &hot, sizeof(hot)); g_hot_host = hot; } }
#endif
    int block = g_ncores < 64 ? g_ncores : 64;       // 2-warp blocks (SM coverage)
    int grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 33 * sizeof(uint32_t);   // 33-stride regfile
    // Translate-on-miss driver: run; if core 0 stopped at an untranslated indirect target, translate
    // that block and resume — until the budget is spent or the guest halts. Static fully-translated
    // guests (the benchmarks) never miss, so this runs exactly once for them.
    long long total = 0;
    // RVX_STATS=1: hybrid launch profile — exec/interp launch counts, retired split, and the wall-time
    // split between GPU work (launch+sync inside this call) and everything else, dumped at exit.
    static long long s_xl=0, s_il=0, s_xret=0, s_iret=0; static double s_tx=0, s_tall=0; static int s_stats=-1;
    if (s_stats < 0) { s_stats = getenv("RVX_STATS") ? 1 : 0;
        if (s_stats) atexit([]{ fprintf(stderr,"[hyb] exec launches %lld (retired %lld), interp launches %lld (retired %lld), gpu %.0f ms, step_all %.0f ms\n",
                                        s_xl, s_xret, s_il, s_iret, s_tx*1e3, s_tall*1e3); }); }
    auto qpc=[]{ LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart; };
    static double s_qpf = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    double t_all = s_stats ? qpc() : 0;
    for (int guard = 0; guard < 1 << 20; guard++) {
        int rem = budget - (int)total;
        if (rem <= 0) break;
        // exec_block fast path: if pc is a cross-compiled word, run the register-resident PTX kernel
        // (no per-uop interpreter tax). It returns at the first uncompiled pc / budget exhaustion;
        // the interpreter then owns whatever it handed back. On launch fault, disable & fall through.
        if (g_xblk_ok) {
            uint32_t xpc = g_state[0].pc;
            int xw = (xpc >= g_base) ? (int)((xpc - g_base) >> 2) : -1;
            if (xw >= 0 && xw < g_pc2words && g_xtab[xw]) {
                double t0 = s_stats ? qpc() : 0;
                long long did = rvxblk_step(rem);
                if (s_stats) { s_tx += (qpc()-t0)/s_qpf; }
                if (did < 0) { g_xblk_ok = 0; }            // exec error → permanently fall back to interpreter
                else {
                    if (s_stats) { s_xl++; s_xret += did; }
                    total += did;
                    uint32_t epc = g_state[0].pc;
                    if (epc & 0x80000000u) break;            // halted
                    // exec_block can hand back at a word that is NOT a valid interpreter leader (e.g. the
                    // fall-through interior of a fused uop → RC_BADUOP). The interpreter HALTS if it *starts*
                    // a launch on a BADUOP pc (ui<0 ⇒ loop never runs ⇒ resume stays HALT_BIT), so translate
                    // that pc into a real leader first — otherwise the guest wrongly appears halted.
                    int ew = (epc >= g_base) ? (int)((epc - g_base) >> 2) : -1;
                    if (ew >= 0 && ew < g_pc2words && g_pc2uop_h[ew] == RC_BADUOP) rvcud_translate_miss(epc);
                    // Single-core: re-evaluate exec vs interpreter for the new pc immediately.
                    // Multi-core: fall THROUGH to the interpreter — cores whose pc was not an exec
                    // entry made no progress in that launch; the interp launch advances them (cores
                    // at entry pcs self-stop after ≥1 instruction, so it costs the fast cores little).
                    if (g_ncores == 1) continue;
                }
            }
        }
        int xstop = g_xblk_ok ? 0x80 : 0;             // self-stop only while exec is live (else: no relaunch storm)
        if (g_ncores == 1)
            rvcud_kernel<true ><<<grid, block, shmem>>>(g_state, g_mem, g_uops, g_uw, g_pc2uop, g_uop2pc, g_ext,
                                                        g_ncores, g_pc2words, g_base, rem, xstop, g_ret);
        else
            rvcud_kernel<false><<<grid, block, shmem>>>(g_state, g_mem, g_uops, g_uw, g_pc2uop, g_uop2pc, g_ext,
                                                        g_ncores, g_pc2words, g_base, rem, xstop, g_ret);
        cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
        if (le != cudaSuccess) return (int)le;
        if (se != cudaSuccess) return (int)se;
        if (s_stats) { s_il++; s_iret += (long long)*g_ret; }
        total += (long long)*g_ret;                  // core 0 retired this launch
        uint32_t pc = g_state[0].pc;                 // managed memory → host-readable
        { static int s_trc = -1; if (s_trc < 0) s_trc = getenv("RVCUD_TRACE") ? 1 : 0;
          if (s_trc) fprintf(stderr, "[trc] pc=%08X ret=%llu total=%lld\n", pc, *g_ret, total); }
        if (pc & 0x80000000u) break;                 // halted (illegal instr / guest done)
        int w = (pc >= g_base) ? (int)((pc - g_base) >> 2) : -1;
        if (w >= 0 && w < g_pc2words && g_pc2uop_h[w] == RC_BADUOP) {
            if (rvcud_translate_miss(pc)) continue;   // translated the missed block → resume
            break;                                    // untranslatable pc — bail
        }
        continue;                                     // interp self-stopped at an exec entry (or budget end) → re-evaluate
    }
    if (s_stats) s_tall += (qpc()-t_all)/s_qpf;
    *g_ret = (unsigned long long)total;               // report TOTAL retired (verify gate reads this)
    return 0;
}

// Guest-instructions retired by core 0 in the last cuda_rvcud_step_all (≈ budget + overshoot,
// since a fused uop retires several). The verify gate runs rv32i for exactly this many.
API unsigned long long cuda_rvcud_retired() { return g_ret ? *g_ret : 0ull; }
API unsigned long long cuda_rvcud_iters() { unsigned long long h=0; cudaMemcpyFromSymbol(&h,g_dev_iters,sizeof(h)); return h; }

// ============================================================================
//  TIERED exec_block: cross-compile a BOUNDED set of hot rv32i blocks to ONE PTX
//  kernel (guest regs → PTX registers, no per-uop interpreter tax) and run it via
//  the driver; the interpreter handles everything not compiled (shared g_state/
//  g_mem, hand off via pc). Size-capped reachability avoids the whole-program
//  ptxas blowup. lw/sw are misalignment-safe (a misaligned fault is unrecoverable).
// ============================================================================
static void rvx_app(std::string& s, const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); s += b;
}
// Cross-instruction codegen state, valid ONLY along a fall-through run that cannot be entered
// sideways (rvx_codegen resets it at every possible branch target / region-entry brx word).
//  - abBase: guest reg cached in %ab == %M + zext(x[abBase]); an imm==0 access off the unchanged
//    base reuses [%ab] directly (bit-exact: same inputs as a fresh compute).
//  - a0Base/a0Imm: %a0 == %M + zext(x[a0Base]+a0Imm); an IDENTICAL (base,imm) repeat reuses %a0
//    (exact incl. 32-bit wrap — same rs1 value, same imm). t0Valid: %t0 still == x[a0Base]+a0Imm.
//  - mod4[r]: x[r] mod 4 (0..3) or 0xFF UNKNOWN. A PROVEN (mod4[rs1]+imm)%align==0 drops the runtime
//    misalignment check. Soundness over coverage: all regs START UNKNOWN (sp is never trusted).
struct RvxCse {
    int abBase, a0Base; int32_t a0Imm; bool t0Valid;
    uint8_t mod4[32];
    uint8_t spInit = 0xFF;     // 0 when the GLOBAL sp-alignment proof holds (see rvxblk_build) — sp
                               // then re-seeds as ALIGNED at every reset, so stack spills/reloads
                               // (the largest class of unproven memops) emit bare ld/st.
    void reset() { abBase = a0Base = -1; t0Valid = false; memset(mod4, 0xFF, sizeof mod4); mod4[0] = 0; mod4[2] = spInit; }
};
// True if `in` either does not write x2 (sp) or provably leaves it 4-aligned given it was:
// the induction step of the global sp-alignment proof. Invalid encodings (image data the
// translator scanned) raise an illegal-instruction halt and write nothing — safe.
static bool rvx_sp_writer_ok(uint32_t in) {
    uint32_t op = in & 0x7F, rd = (in >> 7) & 0x1F, f3 = (in >> 12) & 7, rs1 = (in >> 15) & 0x1F;
    switch (op) {
    case 0x23: case 0x63: return true;                            // store/branch: no rd write (rd bits are imm)
    case 0x37: case 0x17: return true;                            // lui (low 12 = 0) / auipc (pc 4-aligned)
    case 0x6F: case 0x67: return true;                            // jal/jalr link value pc+4 is 4-aligned
    case 0x13:
        if (rd != 2) return true;
        if (f3 == 0) return rs1 == 2 && (rv_iimm(in) & 3) == 0;   // addi sp, sp, 4k
        if (f3 == 7) return (rv_iimm(in) & 3) == 0;               // andi …, mask clearing the low bits
        return false;
    case 0x03: case 0x33: return rd != 2;                         // load / reg-ALU into sp → unprovable
    case 0x73: return true;                                       // ecall/ebreak/CSR ALL halt here (no Zicsr) — no rd write
    default:   return true;                                       // not RV32I → illegal-instr halt, writes nothing
    }
}
// Emit PTX for one guest instruction. Guest regs are %x0..%x31 (%x0 holds 0); %t0/%t1 b32, %a0/%ab b64
// temps; %p0 pred; %M global-mem base. Memory is byte-wise assembled (ncores==1 ⇒ linear layout):
// unless alignment is statically PROVEN via cse.mod4, every load/store runtime-checks and falls back
// to ld/st.global.u8 so a misaligned guest address can never raise the unrecoverable misaligned-access
// fault that aligned ld.global.u32 would. Branch/jal to a SAME-REGION
// compiled FORWARD target → direct bra L<t>; BACKWARD (loop) → inline budget check + bra; jalr → the
// region-local XDISP (same-region targets stay in-register). Cross-region or uncompiled target → set pc,
// bra XSAVE (the region epilogue spills to XS and returns; the dispatcher re-enters the right region
// on-device, or exits to the host/interpreter when pc2idx has no entry for pc).
static void rvx_emit(std::string& s, uint32_t pc, uint32_t instr,
                     const std::vector<uint8_t>& comp, const std::vector<uint8_t>& leader, int N, uint32_t base,
                     const std::vector<int>& regof, int myreg, RvxCse& cse, int nc,
                     const std::vector<uint32_t>& p2i, std::string& cold) {
    const uint32_t op=instr&0x7F, rd=(instr>>7)&0x1F, f3=(instr>>12)&7, rs1=(instr>>15)&0x1F, rs2=(instr>>20)&0x1F, f7=(instr>>25)&0x7F;
    auto goable=[&](uint32_t t)->int{ if(t<base) return 0; uint32_t w=(t-base)>>2;
        return ((t-base)&3)==0 && (int)w<N && comp[w] && leader[w] && regof[w]==myreg; };
    // Emit a transfer to guest target t. cond=true ⇒ guarded by %p0 (conditional branch). Forward
    // same-region target → direct bra (can't loop, no budget check). Backward same-region target (a
    // loop) → INLINE register-only budget check + direct bra: this avoids the per-iteration pc2idx
    // global load + brx.idx that routing through XDISP would cost on every loop trip.
    auto xfer=[&](bool cond, uint32_t t){
        const char* pg = cond ? "@%p0 " : "";
        if(goable(t) && t>pc)      rvx_app(s,"%sbra L%u;\n",pg,t);
        else if(goable(t)) {                                                                          // backward (loop)
            if(cond) rvx_app(s,"setp.lt.and.s32 %%p1, %%cnt, %%budget, %%p0;\nsetp.ge.and.s32 %%p2, %%cnt, %%budget, %%p0;\n"
                               "@%%p2 mov.u32 %%pc, %u;\n@%%p2 bra XSAVE;\n@%%p1 bra L%u;\n",t,t);
            else     rvx_app(s,"setp.ge.s32 %%p0, %%cnt, %%budget;\n@%%p0 mov.u32 %%pc, %u;\n@%%p0 bra XSAVE;\nbra L%u;\n",t,t);
        }
        else                       rvx_app(s,"%smov.u32 %%pc, %u;\n%sbra XSAVE;\n",pg,t,pg);          // leaves region/compiled set
    };
    // Register write: update the alignment lattice (m = new mod4 or 0xFF) and kill any cached address
    // derived from r. Call AFTER any cse.mod4[] reads for this instruction (sources are pre-write values).
    auto wr=[&](uint32_t r, int m){ if(!r) return; cse.mod4[r]=(uint8_t)m;
        if(cse.abBase==(int)r) cse.abBase=-1;
        if(cse.a0Base==(int)r){ cse.a0Base=-1; cse.t0Valid=false; } };
    // Guest byte address rs1+imm. Sets A = b64 operand for [A] (and [A+k] when nc==1), T = b32 reg
    // holding the guest address (runtime alignment check), alok = (x[rs1]+imm)%align==0 statically
    // proven. nc==1 ⇒ flat layout: dev = %M + a. nc>1 ⇒ word-interleaved (word w of core c at
    // g_mem[w*nc+c]): dev = %M' + (a&~3)*nc + (a&3), where %M (= M + 4*core) is pre-offset by the
    // dispatcher. Either way %a0/%ab cache the DEV byte address of a — the CSE reuse logic is
    // layout-independent.
    char A[8]="%a0", T[8]="%t0"; bool alok=false;
    auto addr=[&](int32_t imm, int align){
        alok = (align<=1) || (cse.mod4[rs1]!=0xFF &&
                              ((((uint32_t)cse.mod4[rs1]+(uint32_t)imm) & (uint32_t)(align-1)) == 0));
        strcpy(A,"%a0"); strcpy(T,"%t0");
        if (cse.a0Base==(int)rs1 && cse.a0Imm==imm) {                       // identical repeat → reuse %a0
            if (!alok && !cse.t0Valid) { rvx_app(s,"add.s32 %%t0, %%x%u, %d;\n",rs1,imm); cse.t0Valid=true; }
            return;
        }
        if (imm==0 && cse.abBase==(int)rs1) {                               // same base, imm 0 → [%ab]
            strcpy(A,"%ab"); snprintf(T,sizeof T,"%%x%u",rs1); return;      // guest addr IS x[rs1]
        }
        rvx_app(s,"add.s32 %%t0, %%x%u, %d;\n",rs1,imm);
        if (nc == 1)
            s += "cvt.u64.u32 %a0, %t0;\nadd.u64 %a0, %a0, %M;\n";
        else if (alok && align==4)                                          // a%4==0 proven ⇒ (a&~3)==a, (a&3)==0
            rvx_app(s,"mul.wide.u32 %%a0, %%t0, %d;\nadd.u64 %%a0, %%a0, %%M;\n",nc);
        else
            rvx_app(s,"and.b32 %%t1, %%t0, -4;\nmul.wide.u32 %%a0, %%t1, %d;\n"
                      "and.b32 %%t1, %%t0, 3;\ncvt.u64.u32 %%a1, %%t1;\n"
                      "add.u64 %%a0, %%a0, %%a1;\nadd.u64 %%a0, %%a0, %%M;\n",nc);
        cse.a0Base=(int)rs1; cse.a0Imm=imm; cse.t0Valid=true;
        if (imm==0) { s += "mov.u64 %ab, %a0;\n"; cse.abBase=(int)rs1; }    // seed the base cache
    };
    // Address operand for byte i (i≥1) of a misaligned byte-wise fallback, emitted under @!%p0.
    // Flat: the bytes are adjacent → textual [A+i]. Interleaved: byte a+i may live in the NEXT word
    // (nc*4 bytes away) → recompute its device address into %a1 (clobbers %t1/%t2/%ad as scratch).
    char FB[12];
    auto fbaddr=[&](int i)->const char*{
        if (nc == 1) { snprintf(FB,sizeof FB,"%s+%d",A,i); return FB; }
        rvx_app(s,"@!%%p0 add.s32 %%t2, %s, %d;\n"
                  "@!%%p0 and.b32 %%t1, %%t2, -4;\n@!%%p0 mul.wide.u32 %%a1, %%t1, %d;\n"
                  "@!%%p0 and.b32 %%t2, %%t2, 3;\n@!%%p0 cvt.u64.u32 %%ad, %%t2;\n"
                  "@!%%p0 add.u64 %%a1, %%a1, %%ad;\n@!%%p0 add.u64 %%a1, %%a1, %%M;\n", T, i, nc);
        strcpy(FB,"%a1"); return FB;
    };
    // Shadow return stack push: a call (jal/jalr with rd=ra) records (return pc, the return site's
    // baked pc2idx token) in the per-thread shared stack; the matching ret pops and re-dispatches
    // through the region brx with no global-memory touch. 16 entries with wraparound — overflow
    // just loses the oldest prediction (that ret misses to XDISP; the validating compare keeps it sound).
    auto spush=[&](){
        uint32_t wn = (pc + 4 - base) >> 2;
        uint32_t tok = (wn < (uint32_t)N) ? p2i[wn] : 0xFFFFFFFFu;
        rvx_app(s,"mad.wide.u32 %%a1, %%rsp, 8, %%ss;\n"
                  "mov.b32 %%t1, %u;\nst.shared.u32 [%%a1], %%t1;\n"
                  "mov.b32 %%t1, %u;\nst.shared.u32 [%%a1+4], %%t1;\n"
                  "add.u32 %%rsp, %%rsp, 1;\nand.b32 %%rsp, %%rsp, 15;\n", pc+4, tok);
    };
    switch(op) {
    case 0x37: if(rd){ rvx_app(s,"mov.b32 %%x%u, %u;\n",rd,instr&0xFFFFF000u); wr(rd,0); } break;     // low 12 bits zero
    case 0x17: if(rd){ uint32_t v=pc+(instr&0xFFFFF000u); rvx_app(s,"mov.b32 %%x%u, %u;\n",rd,v); wr(rd,(int)(v&3)); } break;
    case 0x6F:{ uint32_t t=pc+rv_jimm(instr);
                if(rd){ rvx_app(s,"mov.b32 %%x%u, %u;\n",rd,pc+4); wr(rd,(int)((pc+4)&3)); }
                if(rd==1) spush(); xfer(false,t); } break;
    case 0x67:{ cse.t0Valid=false;
                if (rd==0 && rs1==1 && (int)rv_iimm(instr)==0) {
                    // ret (jalr x0, ra, 0): SHADOW-STACK fast path. Pop the predicted (return pc,
                    // dispatch token) pushed by the matching call; on a hit, re-enter through the
                    // region-local brx directly — skipping XDISP's bounds checks and, crucially, its
                    // ~200-cycle GLOBAL pc2idx load. A miss (setjmp/longjmp, stack skew after an
                    // interpreter interlude, post-launch cold stack) falls back to XDISP — the
                    // compare makes the prediction sound, never trusted.
                    s += "and.b32 %t0, %x1, 4294967294;\nmov.b32 %pc, %t0;\n"
                         "sub.u32 %rsp, %rsp, 1;\nand.b32 %rsp, %rsp, 15;\n"
                         "mad.wide.u32 %a1, %rsp, 8, %ss;\n"
                         "ld.shared.u32 %t1, [%a1];\nld.shared.u32 %bidx, [%a1+4];\n"
                         "setp.ne.u32 %p0, %t0, %t1;\n@%p0 bra XDISP;\n"
                         "setp.ge.s32 %p0, %cnt, %budget;\n@%p0 bra XSAVE;\n";   // call/ret loops must still yield
                    rvx_app(s,"shr.u32 %%rg, %%bidx, 20;\nsetp.ne.u32 %%p0, %%rg, %d;\n@%%p0 bra XSAVE;\n", myreg);
                    s += "and.b32 %bidx, %bidx, 1048575;\nbra JBRX;\n";
                    break;
                }
                rvx_app(s,"add.s32 %%t0, %%x%u, %d;\nand.b32 %%t0, %%t0, 4294967294;\n",rs1,(int)rv_iimm(instr));
                if(rd){ rvx_app(s,"mov.b32 %%x%u, %u;\n",rd,pc+4); wr(rd,(int)((pc+4)&3)); }
                if(rd==1) spush();                                  // indirect CALL: arm the callee's ret
                s += "mov.b32 %pc, %t0;\nbra XDISP;\n"; } break;
    case 0x63:{ uint32_t t=pc+rv_bimm(instr); const char* cc; bool sg=false;
                switch(f3){case 0:cc="eq";break;case 1:cc="ne";break;case 4:cc="lt";sg=true;break;case 5:cc="ge";sg=true;break;case 6:cc="lt";break;default:cc="ge";}
                rvx_app(s,"setp.%s.%s %%p0, %%x%u, %%x%u;\n",cc,sg?"s32":"u32",rs1,rs2);
                xfer(true,t); } break;                                                               // not-taken falls through to next emitted word
    case 0x03:{ if(!rd) break;
                { int32_t im=(int)rv_iimm(instr);
                  int aln = f3==2?4:(f3==1||f3==5)?2:1;
                  bool alk = aln>1 && cse.mod4[rs1]!=0xFF &&
                             ((((uint32_t)cse.mod4[rs1]+(uint32_t)im)&(uint32_t)(aln-1))==0);
                  if ((f3==2||f3==1||f3==5) && !alk && nc==1) {
                      // lw/lh/lhu, alignment unproven, flat layout: BRANCH-FREE funnel load — the two
                      // aligned words overlapping the address (the +8 B buffer guard makes the high
                      // word safe) and one shf.r extract. No predication, no fault possible, both
                      // loads issue independently; the checked path was 10-14 instructions with a
                      // predicated byte-wise fallback.
                      rvx_app(s,"add.s32 %%t0, %%x%u, %d;\n"
                                "and.b32 %%t1, %%t0, -4;\ncvt.u64.u32 %%a1, %%t1;\nadd.u64 %%a1, %%a1, %%M;\n"
                                "ld.global.u32 %%t1, [%%a1];\nld.global.u32 %%t2, [%%a1+4];\n"
                                "and.b32 %%t0, %%t0, 3;\nshl.b32 %%t0, %%t0, 3;\n", rs1, im);
                      if      (f3==2) rvx_app(s,"shf.r.wrap.b32 %%x%u, %%t1, %%t2, %%t0;\n", rd);
                      else if (f3==5) rvx_app(s,"shf.r.wrap.b32 %%t1, %%t1, %%t2, %%t0;\nand.b32 %%x%u, %%t1, 65535;\n", rd);
                      else            rvx_app(s,"shf.r.wrap.b32 %%t1, %%t1, %%t2, %%t0;\ncvt.s32.s16 %%x%u, %%t1;\n", rd);
                      cse.t0Valid=false; wr(rd,0xFF); break;
                  } }
                addr((int)rv_iimm(instr), f3==2?4:(f3==1||f3==5)?2:1); wr(rd,0xFF);     // loaded value: unknown mod 4
                if(f3==0){ rvx_app(s,"ld.global.s8 %%x%u, [%s];\n",rd,A); break; }                     // lb (always 1-aligned)
                if(f3==4){ rvx_app(s,"ld.global.u8 %%x%u, [%s];\n",rd,A); break; }                     // lbu
                if(f3==1||f3==5){ // lh/lhu: proven-aligned → bare ld; else runtime check + byte-wise fallback
                    const char* sty=(f3==1)?"s16":"u16"; const char* hib=(f3==1)?"s8":"u8";
                    if(alok){ rvx_app(s,"ld.global.%s %%x%u, [%s];\n",sty,rd,A); break; }
                    rvx_app(s,"and.b32 %%t1, %s, 1;\nsetp.eq.u32 %%p0, %%t1, 0;\n"
                              "@%%p0 ld.global.%s %%x%u, [%s];\n"
                              "@!%%p0 ld.global.u8 %%x%u, [%s];\n",
                              T, sty,rd,A, rd,A);
                    rvx_app(s,"@!%%p0 ld.global.%s %%t1, [%s];\n@!%%p0 shl.b32 %%t1, %%t1, 8;\n@!%%p0 or.b32 %%x%u, %%x%u, %%t1;\n",
                              hib, fbaddr(1), rd,rd); break; }
                // lw: proven-aligned → bare ld.u32; else aligned fast path + byte-wise fallback — predication
                //     makes the misaligned ld.u32 a no-op, so the unrecoverable misaligned fault can never fire.
                if(alok){ rvx_app(s,"ld.global.u32 %%x%u, [%s];\n",rd,A); break; }
                rvx_app(s,"and.b32 %%t1, %s, 3;\nsetp.eq.u32 %%p0, %%t1, 0;\n"
                          "@%%p0 ld.global.u32 %%x%u, [%s];\n"
                          "@!%%p0 ld.global.u8 %%x%u, [%s];\n",
                          T, rd,A, rd,A);
                for (int i=1;i<4;i++)
                    rvx_app(s,"@!%%p0 ld.global.u8 %%t1, [%s];\n@!%%p0 shl.b32 %%t1, %%t1, %d;\n@!%%p0 or.b32 %%x%u, %%x%u, %%t1;\n",
                              fbaddr(i), 8*i, rd,rd);
              } break;
    case 0x23:{ addr((int)rv_simm(instr), f3==2?4:(f3==1)?2:1);
                if(f3==0){ rvx_app(s,"st.global.u8 [%s], %%x%u;\n",A,rs2); break; }                    // sb
                if(f3==1){ // sh: proven-aligned → bare st; else aligned fast path, byte fallback COLD
                    if(alok){ rvx_app(s,"st.global.u16 [%s], %%x%u;\n",A,rs2); break; }
                    if(nc==1){
                        // Branch-over-fallback: predicated-off instructions still consume issue slots,
                        // so the old form charged the (almost always aligned) store ~5 squashed issues.
                        // The misaligned case jumps to a cold block after the region body.
                        rvx_app(s,"and.b32 %%t1, %s, 1;\nsetp.ne.u32 %%p0, %%t1, 0;\n@%%p0 bra CF%u;\n"
                                  "st.global.u16 [%s], %%x%u;\nCJ%u:\n", T, pc, A,rs2, pc);
                        rvx_app(cold,"CF%u:\nst.global.u8 [%s], %%x%u;\nshr.b32 %%t1, %%x%u, 8;\nst.global.u8 [%s+1], %%t1;\nbra CJ%u;\n",
                                  pc, A,rs2, rs2, A, pc);
                        break; }
                    rvx_app(s,"and.b32 %%t1, %s, 1;\nsetp.eq.u32 %%p0, %%t1, 0;\n"
                              "@%%p0 st.global.u16 [%s], %%x%u;\n"
                              "@!%%p0 st.global.u8 [%s], %%x%u;\n",
                              T, A,rs2, A,rs2);
                    rvx_app(s,"@!%%p0 shr.b32 %%t1, %%x%u, 8;\n@!%%p0 st.global.u8 [%s], %%t1;\n", rs2, fbaddr(1)); break; }
                // sw: proven-aligned → bare st.u32; else aligned fast path, byte fallback COLD (flat layout)
                if(alok){ rvx_app(s,"st.global.u32 [%s], %%x%u;\n",A,rs2); break; }
                if(nc==1){
                    rvx_app(s,"and.b32 %%t1, %s, 3;\nsetp.ne.u32 %%p0, %%t1, 0;\n@%%p0 bra CF%u;\n"
                              "st.global.u32 [%s], %%x%u;\nCJ%u:\n", T, pc, A,rs2, pc);
                    rvx_app(cold,"CF%u:\nst.global.u8 [%s], %%x%u;\n", pc, A,rs2);
                    for (int i=1;i<4;i++)
                        rvx_app(cold,"shr.b32 %%t1, %%x%u, %d;\nst.global.u8 [%s+%d], %%t1;\n", rs2, 8*i, A, i);
                    rvx_app(cold,"bra CJ%u;\n", pc);
                    break; }
                rvx_app(s,"and.b32 %%t1, %s, 3;\nsetp.eq.u32 %%p0, %%t1, 0;\n"
                          "@%%p0 st.global.u32 [%s], %%x%u;\n"
                          "@!%%p0 st.global.u8 [%s], %%x%u;\n",
                          T, A,rs2, A,rs2);
                for (int i=1;i<4;i++)
                    rvx_app(s,"@!%%p0 shr.b32 %%t1, %%x%u, %d;\n@!%%p0 st.global.u8 [%s], %%t1;\n", rs2, 8*i, fbaddr(i));
              } break;
    case 0x13:{ if(!rd) break; int im=(int)rv_iimm(instr); uint32_t sh=im&0x1F;
                int nm = 0xFF;                                                          // lattice: addi propagates, slli>=2 zeroes
                if (f3==0 && cse.mod4[rs1]!=0xFF) nm = (int)(((uint32_t)cse.mod4[rs1]+(uint32_t)im)&3);
                else if (f3==1 && sh>=2) nm = 0;
                switch(f3){
                  case 0: rvx_app(s,"add.s32 %%x%u, %%x%u, %d;\n",rd,rs1,im); break;
                  case 2: rvx_app(s,"setp.lt.s32 %%p0, %%x%u, %d;\nselp.b32 %%x%u, 1, 0, %%p0;\n",rs1,im,rd); break;
                  case 3: rvx_app(s,"setp.lt.u32 %%p0, %%x%u, %u;\nselp.b32 %%x%u, 1, 0, %%p0;\n",rs1,(uint32_t)im,rd); break;
                  case 4: rvx_app(s,"xor.b32 %%x%u, %%x%u, %u;\n",rd,rs1,(uint32_t)im); break;
                  case 6: rvx_app(s,"or.b32 %%x%u, %%x%u, %u;\n",rd,rs1,(uint32_t)im); break;
                  case 7: rvx_app(s,"and.b32 %%x%u, %%x%u, %u;\n",rd,rs1,(uint32_t)im); break;
                  case 1: rvx_app(s,"shl.b32 %%x%u, %%x%u, %u;\n",rd,rs1,sh); break;
                  default: rvx_app(s,"shr.%s %%x%u, %%x%u, %u;\n",(f7==0x20)?"s32":"u32",rd,rs1,sh); }
                wr(rd,nm); } break;
    case 0x33:{ if(!rd) break;
                if (f7==0x01) {                                                         // M extension (exact RV32M semantics)
                    switch(f3){
                      case 0: rvx_app(s,"mul.lo.s32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); break;
                      case 1: rvx_app(s,"mul.hi.s32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); break;
                      case 2: rvx_app(s,"cvt.s64.s32 %%a0, %%x%u;\ncvt.u64.u32 %%a1, %%x%u;\n"   // mulhsu: (s64)x * (u64)y >> 32
                                        "mul.lo.s64 %%a0, %%a0, %%a1;\nshr.u64 %%a0, %%a0, 32;\n"
                                        "cvt.u32.u64 %%x%u, %%a0;\n",rs1,rs2,rd); break;
                      case 3: rvx_app(s,"mul.hi.u32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); break;
                      case 4: // div: y==0 → -1; INT_MIN/-1 → INT_MIN (divisor forced to 1 covers it: INT_MIN/1)
                              rvx_app(s,"setp.eq.s32 %%p0, %%x%u, 0;\n"
                                        "setp.eq.s32 %%p1, %%x%u, -2147483648;\nsetp.eq.s32 %%p2, %%x%u, -1;\n"
                                        "and.pred %%p1, %%p1, %%p2;\nor.pred %%p2, %%p0, %%p1;\n"
                                        "selp.b32 %%t0, 1, %%x%u, %%p2;\n"
                                        "div.s32 %%t1, %%x%u, %%t0;\n"
                                        "selp.b32 %%x%u, -1, %%t1, %%p0;\n",
                                        rs2, rs1, rs2, rs2, rs1, rd); cse.t0Valid=false; break;
                      case 5: rvx_app(s,"setp.eq.u32 %%p0, %%x%u, 0;\nselp.b32 %%t0, 1, %%x%u, %%p0;\n"
                                        "div.u32 %%t1, %%x%u, %%t0;\nselp.b32 %%x%u, -1, %%t1, %%p0;\n",
                                        rs2, rs2, rs1, rd); cse.t0Valid=false; break;
                      case 6: // rem: y==0 → x; INT_MIN%-1 → 0 (x%1 == 0 covers it)
                              rvx_app(s,"setp.eq.s32 %%p0, %%x%u, 0;\n"
                                        "setp.eq.s32 %%p1, %%x%u, -2147483648;\nsetp.eq.s32 %%p2, %%x%u, -1;\n"
                                        "and.pred %%p1, %%p1, %%p2;\nor.pred %%p2, %%p0, %%p1;\n"
                                        "selp.b32 %%t0, 1, %%x%u, %%p2;\n"
                                        "rem.s32 %%t1, %%x%u, %%t0;\n"
                                        "selp.b32 %%x%u, %%x%u, %%t1, %%p0;\n",
                                        rs2, rs1, rs2, rs2, rs1, rd, rs1); cse.t0Valid=false; break;
                      default:rvx_app(s,"setp.eq.u32 %%p0, %%x%u, 0;\nselp.b32 %%t0, 1, %%x%u, %%p0;\n"
                                        "rem.u32 %%t1, %%x%u, %%t0;\nselp.b32 %%x%u, %%x%u, %%t1, %%p0;\n",
                                        rs2, rs2, rs1, rd, rs1); cse.t0Valid=false; break;
                    }
                    wr(rd,0xFF); break;
                }
                int nm = 0xFF;                                                          // lattice: add/sub combine if both known
                if (f3==0 && cse.mod4[rs1]!=0xFF && cse.mod4[rs2]!=0xFF)
                    nm = (int)(((f7==0x20) ? (uint32_t)cse.mod4[rs1]-(uint32_t)cse.mod4[rs2]
                                           : (uint32_t)cse.mod4[rs1]+(uint32_t)cse.mod4[rs2]) & 3);
                switch(f3){
                  case 0: rvx_app(s,"%s.s32 %%x%u, %%x%u, %%x%u;\n",(f7==0x20)?"sub":"add",rd,rs1,rs2); break;
                  case 1: rvx_app(s,"and.b32 %%t0, %%x%u, 31;\nshl.b32 %%x%u, %%x%u, %%t0;\n",rs2,rd,rs1); cse.t0Valid=false; break;
                  case 2: rvx_app(s,"setp.lt.s32 %%p0, %%x%u, %%x%u;\nselp.b32 %%x%u, 1, 0, %%p0;\n",rs1,rs2,rd); break;
                  case 3: rvx_app(s,"setp.lt.u32 %%p0, %%x%u, %%x%u;\nselp.b32 %%x%u, 1, 0, %%p0;\n",rs1,rs2,rd); break;
                  case 4: rvx_app(s,"xor.b32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); break;
                  case 5: rvx_app(s,"and.b32 %%t0, %%x%u, 31;\nshr.%s %%x%u, %%x%u, %%t0;\n",rs2,(f7==0x20)?"s32":"u32",rd,rs1); cse.t0Valid=false; break;
                  case 6: rvx_app(s,"or.b32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); break;
                  default: rvx_app(s,"and.b32 %%x%u, %%x%u, %%x%u;\n",rd,rs1,rs2); }
                wr(rd,nm); } break;
    case 0x0F: break;                                                                     // fence → nop
    default: rvx_app(s,"mov.u32 %%pc, %u;\nbra XSAVE;\n",pc|0x80000000u); break;           // system/illegal → halt
    }
}

// Ops rvx_emit can fully execute in-kernel (everything else — system/illegal — is a hand-off to the
// interpreter). 0x73 (ecall/ebreak/CSR) is deliberately EXCLUDED so the interpreter keeps owning traps.
static inline bool rvx_compilable(uint32_t op){
    return op==0x37||op==0x17||op==0x6F||op==0x67||op==0x63||op==0x03||op==0x23||op==0x13||op==0x33||op==0x0F;
}

// Bit-serial restoring-divide loop shape (mirror of rvcud_try_divloop's body conditions; see that
// matcher for per-instruction commentary). __udivsi3/__divsi3/__umodsi3 dominate Doom's boot path
// (~27% of guest-pc samples): the 13-instruction loop runs exactly 32 iterations, so the region
// codegen replaces it with ONE hardware div+rem — straight-line, no new loop shape (knee-light).
// A runtime precondition test (R==0 ∧ Q==0 ∧ i==31, i.e. the canonical entry state the prologue
// establishes) routes any mid-loop sideways re-entry to the original code, which stays emitted.
struct RvxDiv { uint32_t Q,R,N,D,I; };
static bool rvx_match_divloop(const uint32_t* img, int Nw, uint32_t base, int w, RvxDiv& o) {
    if (w+13 >= Nw) return false;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;};
    const uint32_t* i = img + w;
    if ((i[0]&0x7F)!=0x13 || F3(i[0])!=1 || S2(i[0])!=1) return false;            // slli R,R,1
    uint32_t R=RD(i[0]); if (S1(i[0])!=R || R==0) return false;
    if ((i[1]&0x7F)!=0x33 || F3(i[1])!=5 || F7(i[1])!=0) return false;            // srl T1,N,i
    uint32_t T1=RD(i[1]), Nr=S1(i[1]), ir=S2(i[1]);
    if ((i[2]&0x7F)!=0x33 || F3(i[2])!=1 || F7(i[2])!=0 || S2(i[2])!=ir) return false;   // sll T2,ONE,i
    uint32_t T2=RD(i[2]);
    if ((i[3]&0x7F)!=0x13 || F3(i[3])!=0 || RD(i[3])!=ir || S1(i[3])!=ir || (int32_t)rv_iimm(i[3])!=-1) return false;
    if ((i[4]&0x7F)!=0x13 || F3(i[4])!=7 || RD(i[4])!=T1 || S1(i[4])!=T1 || rv_iimm(i[4])!=1) return false;
    if ((i[5]&0x7F)!=0x33 || F3(i[5])!=6 || F7(i[5])!=0 || RD(i[5])!=R) return false;
    if (!((S1(i[5])==T1&&S2(i[5])==R)||(S1(i[5])==R&&S2(i[5])==T1))) return false;
    if ((i[6]&0x7F)!=0x33 || F3(i[6])!=3 || F7(i[6])!=0 || RD(i[6])!=T1 || S1(i[6])!=R) return false;
    uint32_t Dr=S2(i[6]);
    if ((i[7]&0x7F)!=0x13 || F3(i[7])!=0 || RD(i[7])!=T1 || S1(i[7])!=T1 || (int32_t)rv_iimm(i[7])!=-1) return false;
    if ((i[8]&0x7F)!=0x33 || F3(i[8])!=7 || F7(i[8])!=0 || RD(i[8])!=T2) return false;
    if (!((S1(i[8])==T1&&S2(i[8])==T2)||(S1(i[8])==T2&&S2(i[8])==T1))) return false;
    if ((i[9]&0x7F)!=0x33 || F3(i[9])!=7 || F7(i[9])!=0 || RD(i[9])!=T1) return false;
    if (!((S1(i[9])==T1&&S2(i[9])==Dr)||(S1(i[9])==Dr&&S2(i[9])==T1))) return false;
    if ((i[10]&0x7F)!=0x33 || F3(i[10])!=6 || F7(i[10])!=0) return false;
    uint32_t Q=RD(i[10]);
    if (!((S1(i[10])==T2&&S2(i[10])==Q)||(S1(i[10])==Q&&S2(i[10])==T2))) return false;
    if ((i[11]&0x7F)!=0x33 || F3(i[11])!=0 || F7(i[11])!=0x20 || RD(i[11])!=R || S1(i[11])!=R || S2(i[11])!=T1) return false;
    if ((i[12]&0x7F)!=0x63 || F3(i[12])!=1) return false;                          // bne i,NEG1,→top
    if (S1(i[12])!=ir && S2(i[12])!=ir) return false;
    uint32_t bpc=base+(uint32_t)(w+12)*4;
    if ((int)((bpc+rv_bimm(i[12])-base)>>2) != w) return false;
    if (R==0||Nr==0||Dr==0||ir==0||Q==0||T1==0||T2==0) return false;
    if (R==Nr||R==Dr||R==Q||R==ir||Nr==Dr||Q==ir||T1==T2||R==T1||R==T2) return false;
    if (Q==Nr||Q==Dr) return false;                    // Q write must not clobber the div inputs mid-replacement
    o.Q=Q; o.R=R; o.N=Nr; o.D=Dr; o.I=ir;
    return true;
}

// Shift-add software-multiply loop shape (mirror of rvcud_try_mulloop):
//   slli A,M,31; srli M,M,1; srai A,A,31; and A,A,B; add ACC,A,ACC; slli B,B,1; bne M,x0,→top
// computing ACC += B*M. Replaced straight-line by ONE mul.lo (knee-light at the current region
// size). The closed form is valid from ANY entry state, so no precondition test is needed; the
// data-dependent retire (7/iter, it = M ? 32-clz(M) : 1) is computed with clz. Final state mirrors
// the interpreter's RC_MULLOOP arm: ACC += B0*M0; M=0; B=B0<<it (PTX shl clamps ≥32 to 0, matching);
// A = M0 ? B0<<(it-1) : 0.
struct RvxMul { uint32_t A,M,B,ACC; };
static bool rvx_match_mulloop(const uint32_t* img, int Nw, uint32_t base, int w, RvxMul& o) {
    if (w+7 >= Nw) return false;
    const uint32_t* i = img + w;
    if ((i[0]&0x7F)!=0x13 || ((i[0]>>12)&7)!=1 || ((i[0]>>20)&0x1F)!=31) return false;
    uint32_t A=(i[0]>>7)&0x1F, M=(i[0]>>15)&0x1F;
    if ((i[1]&0x7F)!=0x13 || ((i[1]>>12)&7)!=5 || ((i[1]>>30)&1)!=0 || ((i[1]>>20)&0x1F)!=1
        || ((i[1]>>7)&0x1F)!=M || ((i[1]>>15)&0x1F)!=M) return false;
    if ((i[2]&0x7F)!=0x13 || ((i[2]>>12)&7)!=5 || ((i[2]>>30)&1)!=1 || ((i[2]>>20)&0x1F)!=31
        || ((i[2]>>7)&0x1F)!=A || ((i[2]>>15)&0x1F)!=A) return false;
    if ((i[3]&0x7F)!=0x33 || ((i[3]>>25)&0x7F)!=0 || ((i[3]>>12)&7)!=7 || ((i[3]>>7)&0x1F)!=A) return false;
    uint32_t s1=(i[3]>>15)&0x1F, s2=(i[3]>>20)&0x1F, Bb;
    if (s1==A) Bb=s2; else if (s2==A) Bb=s1; else return false;
    if ((i[4]&0x7F)!=0x33 || ((i[4]>>25)&0x7F)!=0 || ((i[4]>>12)&7)!=0) return false;
    uint32_t ACC=(i[4]>>7)&0x1F, a1=(i[4]>>15)&0x1F, a2=(i[4]>>20)&0x1F;
    if (!((a1==A && a2==ACC) || (a2==A && a1==ACC))) return false;
    if ((i[5]&0x7F)!=0x13 || ((i[5]>>12)&7)!=1 || ((i[5]>>20)&0x1F)!=1
        || ((i[5]>>7)&0x1F)!=Bb || ((i[5]>>15)&0x1F)!=Bb) return false;
    if ((i[6]&0x7F)!=0x63 || ((i[6]>>12)&7)!=1) return false;
    uint32_t b1=(i[6]>>15)&0x1F, b2=(i[6]>>20)&0x1F;
    if (!((b1==M && b2==0) || (b2==M && b1==0))) return false;
    uint32_t bpc=base+(uint32_t)(w+6)*4;
    if ((int)((bpc + rv_bimm(i[6]) - base) >> 2) != w) return false;
    if (A==0||M==0||Bb==0||ACC==0 || A==M||A==Bb||A==ACC||M==Bb||M==ACC||Bb==ACC) return false;
    o.A=A; o.M=M; o.B=Bb; o.ACC=ACC;
    return true;
}

// Byte-copy loop shape: lb/lbu RT,0(S); sb RT,0(D); addi D,D,1; addi S,S,1; bne D,END,→top
// (the two addis in either order). This is the present-path screen copy in the boot/menu workload
// (~24% of guest-pc samples, 64000 iterations per frame). Replaced by a CALL into the tiny dedicated
// `xcopy` unit (word-widened funnel copy; knee-ISOLATED — that unit assembles alone in microseconds)
// plus a straight-line closed form for the guest registers: D→END, S+=len, RT=last byte copied.
struct RvxCpy { uint32_t S,D,END,RT; bool sext; };
static bool rvx_match_copyloop(const uint32_t* img, int Nw, uint32_t base, int w, RvxCpy& o) {
    if (w+5 >= Nw) return false;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    const uint32_t* i = img + w;
    if ((i[0]&0x7F)!=0x03 || (F3(i[0])!=0 && F3(i[0])!=4) || rv_iimm(i[0])!=0) return false;
    o.sext = F3(i[0])==0; o.RT=RD(i[0]); o.S=S1(i[0]);
    if ((i[1]&0x7F)!=0x23 || F3(i[1])!=0 || S2(i[1])!=o.RT || rv_simm(i[1])!=0) return false;
    o.D=S1(i[1]);
    auto isInc=[&](uint32_t in, uint32_t r){ return (in&0x7F)==0x13 && F3(in)==0 && RD(in)==r && S1(in)==r && rv_iimm(in)==1; };
    if (isInc(i[2],o.D)      && isInc(i[3],o.S)) {}
    else if (isInc(i[2],o.S) && isInc(i[3],o.D)) {}
    else return false;
    if ((i[4]&0x7F)!=0x63 || F3(i[4])!=1) return false;
    if (S1(i[4])==o.D) o.END=S2(i[4]); else if (S2(i[4])==o.D) o.END=S1(i[4]); else return false;
    uint32_t bpc=base+(uint32_t)(w+4)*4;
    if ((int)((bpc+rv_bimm(i[4])-base)>>2) != w) return false;
    if (o.S==0||o.D==0||o.END==0||o.RT==0) return false;
    if (o.S==o.D||o.S==o.END||o.S==o.RT||o.D==o.END||o.D==o.RT||o.END==o.RT) return false;
    return true;
}

// Fill loop shapes (mirrors of rvcud_try_memset / rvcud_try_wordfill), both replaced by a CALL into
// the tiny dedicated `xfill` unit (pattern-replicated word fill; knee-ISOLATED like xcopy) plus the
// exact closed form D(/nD)=END:
//   form A (4 instrs/iter): addi nD,D,1 ; sb VAL,0(D) ; mv D,nD ; bne nD,END,→top
//   form B (3 instrs/iter): sb/sh/sw VAL,0(D) ; addi D,D,K ; bne D,END,→top — CONTIGUOUS only
//     (K == 1<<f3); strided fills keep the original loop.
struct RvxFill { uint32_t D,VAL,END,ND,f3; int nw; };   // ND=0 for form B; nw = words consumed
static bool rvx_match_fillloop(const uint32_t* img, int Nw, uint32_t base, int w, RvxFill& o) {
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    const uint32_t* i = img + w;
    if (w+4 < Nw && (i[0]&0x7F)==0x13 && F3(i[0])==0 && (int32_t)rv_iimm(i[0])==1) {   // form A
        uint32_t nD=RD(i[0]), D=S1(i[0]);
        if (nD && D && nD!=D
            && (i[1]&0x7F)==0x23 && F3(i[1])==0 && S1(i[1])==D && rv_simm(i[1])==0
            && (i[2]&0x7F)==0x13 && F3(i[2])==0 && RD(i[2])==D && S1(i[2])==nD && rv_iimm(i[2])==0
            && (i[3]&0x7F)==0x63 && F3(i[3])==1) {
            uint32_t VAL=S2(i[1]), b1=S1(i[3]), b2=S2(i[3]);
            uint32_t END = (b1==nD) ? b2 : (b2==nD) ? b1 : 0xFFu;
            uint32_t bpc = base+(uint32_t)(w+3)*4;
            if (END!=0xFFu && END!=0 && (int)((bpc+rv_bimm(i[3])-base)>>2)==w
                && VAL!=D && VAL!=nD && END!=D && END!=nD && VAL!=END) {
                o.D=D; o.VAL=VAL; o.END=END; o.ND=nD; o.f3=0; o.nw=4; return true;
            }
        }
    }
    if (w+3 < Nw && (i[0]&0x7F)==0x23 && F3(i[0])<=2 && rv_simm(i[0])==0) {            // form B
        uint32_t f3=F3(i[0]), D=S1(i[0]), VAL=S2(i[0]);
        if (D!=0 && VAL!=D
            && (i[1]&0x7F)==0x13 && F3(i[1])==0 && RD(i[1])==D && S1(i[1])==D
            && (int32_t)rv_iimm(i[1])==(int32_t)(1u<<f3)
            && (i[2]&0x7F)==0x63 && F3(i[2])==1) {
            uint32_t b1=S1(i[2]), b2=S2(i[2]);
            uint32_t END = (b1==D) ? b2 : (b2==D) ? b1 : 0xFFu;
            uint32_t bpc = base+(uint32_t)(w+2)*4;
            if (END!=0xFFu && END!=0 && END!=D && (int)((bpc+rv_bimm(i[2])-base)>>2)==w) {
                o.D=D; o.VAL=VAL; o.END=END; o.ND=0; o.f3=f3; o.nw=3; return true;
            }
        }
    }
    return false;
}

// Palette-expand loop shape (mirror of rvcud_try_palexp — see that matcher for the per-instruction
// commentary; 14 instrs/iter, the present-path RGBA expansion at 64000 iterations/frame):
//   lbu IDX,0(SRC); addi SRC,SRC,1; slli T,IDX,1; add P,PAL,IDX; add P,P,T; lbu C0,0(P);
//   sb C0,-3(DST); lbu C1,1(P); sb C1,-2(DST); lbu C2,2(P); sb C2,-1(DST); sb ALPHA,0(DST);
//   addi DST,DST,4; bne SRC,END,→top
// Replaced by a CALL into the tiny dedicated `xpal` unit (one word store per pixel; knee-ISOLATED —
// the r11/r13 INLINE emissions of this loop tripped the ptxas knee, the xcopy-style unit does not)
// plus the exact closed form: SRC=END, DST+=4n, temps re-derived from the last pixel in guest write
// order. STRICTER than the interpreter matcher: all 11 registers pairwise distinct and nonzero, so
// the closed form is exact with no aliasing analysis.
struct RvxPal { uint32_t SRC,DST,PAL,END,ALPHA,IDX,T,P,C0,C1,C2; };
static bool rvx_match_palexp(const uint32_t* img, int Nw, uint32_t base, int w, RvxPal& o) {
    if (w+14 >= Nw) return false;
    auto RD=[](uint32_t x){return (x>>7)&0x1F;}; auto S1=[](uint32_t x){return (x>>15)&0x1F;};
    auto S2=[](uint32_t x){return (x>>20)&0x1F;}; auto F3=[](uint32_t x){return (x>>12)&7;};
    auto F7=[](uint32_t x){return (x>>25)&0x7F;};
    const uint32_t* i = img + w;
    if ((i[0]&0x7F)!=0x03 || F3(i[0])!=4 || rv_iimm(i[0])!=0) return false;          // lbu IDX,0(SRC)
    uint32_t IDX=RD(i[0]), SRC=S1(i[0]);
    if ((i[1]&0x7F)!=0x13 || F3(i[1])!=0 || RD(i[1])!=SRC || S1(i[1])!=SRC || rv_iimm(i[1])!=1) return false;
    if ((i[2]&0x7F)!=0x13 || F3(i[2])!=1 || S1(i[2])!=IDX || S2(i[2])!=1) return false;   // slli T,IDX,1
    uint32_t T=RD(i[2]);
    if ((i[3]&0x7F)!=0x33 || F3(i[3])!=0 || F7(i[3])!=0) return false;               // add P,PAL,IDX
    uint32_t P=RD(i[3]), PAL;
    if (S1(i[3])==IDX) PAL=S2(i[3]); else if (S2(i[3])==IDX) PAL=S1(i[3]); else return false;
    if ((i[4]&0x7F)!=0x33 || F3(i[4])!=0 || F7(i[4])!=0 || RD(i[4])!=P) return false;     // add P,P,T
    if (!((S1(i[4])==P&&S2(i[4])==T)||(S1(i[4])==T&&S2(i[4])==P))) return false;
    if ((i[5]&0x7F)!=0x03 || F3(i[5])!=4 || S1(i[5])!=P || rv_iimm(i[5])!=0) return false;
    uint32_t C0=RD(i[5]);
    if ((i[6]&0x7F)!=0x23 || F3(i[6])!=0 || S2(i[6])!=C0 || (int32_t)rv_simm(i[6])!=-3) return false;
    uint32_t DST=S1(i[6]);
    if ((i[7]&0x7F)!=0x03 || F3(i[7])!=4 || S1(i[7])!=P || rv_iimm(i[7])!=1) return false;
    uint32_t C1=RD(i[7]);
    if ((i[8]&0x7F)!=0x23 || F3(i[8])!=0 || S1(i[8])!=DST || S2(i[8])!=C1 || (int32_t)rv_simm(i[8])!=-2) return false;
    if ((i[9]&0x7F)!=0x03 || F3(i[9])!=4 || S1(i[9])!=P || rv_iimm(i[9])!=2) return false;
    uint32_t C2=RD(i[9]);
    if ((i[10]&0x7F)!=0x23 || F3(i[10])!=0 || S1(i[10])!=DST || S2(i[10])!=C2 || (int32_t)rv_simm(i[10])!=-1) return false;
    if ((i[11]&0x7F)!=0x23 || F3(i[11])!=0 || S1(i[11])!=DST || rv_simm(i[11])!=0) return false;
    uint32_t ALPHA=S2(i[11]);
    if ((i[12]&0x7F)!=0x13 || F3(i[12])!=0 || RD(i[12])!=DST || S1(i[12])!=DST || rv_iimm(i[12])!=4) return false;
    if ((i[13]&0x7F)!=0x63 || F3(i[13])!=1) return false;                            // bne SRC,END,→top
    uint32_t END;
    if (S1(i[13])==SRC) END=S2(i[13]); else if (S2(i[13])==SRC) END=S1(i[13]); else return false;
    uint32_t bpc=base+(uint32_t)(w+13)*4;
    if ((int)((bpc+rv_bimm(i[13])-base)>>2) != w) return false;
    // Aliasing rules. The five live regs follow the interpreter matcher (PAL/END/ALPHA may alias
    // each other — all invariant). Temps must not alias any live reg (they'd break an invariant or
    // the loop-carried SRC/DST), and only the temp-temp aliases that violate a read-after-write
    // inside one iteration are excluded: T==IDX (i2 kills IDX before i3), P==T (i3 kills T before
    // i4), C0/C1==P (i5/i7 kill P before i7/i9). Everything else (P==IDX, C2==P, C0==C1==T — all
    // present in clang's actual register allocation) is exact because the closed form re-derives
    // the temps in guest write order.
    uint32_t live[5] = {SRC,DST,PAL,END,ALPHA}, tmp[6] = {IDX,T,P,C0,C1,C2};
    for (int a=0; a<5; a++) if (live[a]==0) return false;
    if (SRC==DST||SRC==PAL||SRC==END||SRC==ALPHA||DST==PAL||DST==END||DST==ALPHA) return false;
    for (int a=0; a<6; a++) { if (tmp[a]==0) return false;
        for (int b=0; b<5; b++) if (tmp[a]==live[b]) return false; }
    if (T==IDX || P==T || C0==P || C1==P) return false;
    o.SRC=SRC; o.DST=DST; o.PAL=PAL; o.END=END; o.ALPHA=ALPHA;
    o.IDX=IDX; o.T=T; o.P=P; o.C0=C0; o.C1=C1; o.C2=C2;
    return true;
}

// MULTIMOD exec_block codegen: K region `.func xr<r>`s + ONE `.entry xk` dispatcher, each its own PTX
// unit (units[0]=dispatcher, units[1+r]=region r), SEPARATELY compiled by the driver JIT and device-
// linked into one module. Why (all measured on this machine, CUDA 13.2 ptxas, a 24000-word guest image):
//   - ONE .entry, one 21611-target brx.idx (the old monolith): 1.38 GB — but on a knife edge: ANY
//     perturbation (+3 instructions, or splitting the brx into 4 in the same entry) → 12-25 GB.
//   - K .funcs in ONE unit: ~12 GB regardless of K (whole-module .func analysis, ~0.5 MB per target).
//   - the SAME region .func compiled ALONE: 0.83 GB. Per-unit compilation is what kills the knee, and
//     it makes total coverage scale ~linearly in K with a FLAT per-unit peak.
// Guest regs live in PTX registers %x1..%x31 WITHIN a region; at region boundaries they spill/load
// through a 168-byte .global block XS (defined in the dispatcher unit, .extern in region units) —
// region transitions are vastly rarer than instructions. Cross-region transfers stay ON-DEVICE: the
// region returns and the dispatcher loop re-enters the target region (a host hand-off per crossing
// would collapse throughput — measured 20× at RVX_MAXW=8000).
//   XS layout (bytes): [4*r] x<r> (r=1..31) | [128] pc | [132] cnt | [136] budget |
//                      [144] M (u64) | [152] P2I (u64) | [160] region-entry brx ordinal
// pc2idx[w] = (region<<20)|local-ordinal for every region-entry word (0xFFFFFFFF = not enterable).
// Region-entry words = `disp` leaders ∪ cross-region static-edge targets — the latter keep region-
// boundary branches enterable. Within a region, labels / fall-through / direct bra / inline-budget
// backward branches work exactly like the old monolith; jalr probes its own region via the local XDISP.
// Region size: BIG regions win — every cross-region transition costs a 31-reg spill/reload through
// shared + a dispatcher round trip, and the measured ttf30 climbs monotonically with region size
// (6000→12000→20000→40000→60000: 121.5→126→129→133→139.6 MIPS, all interleaved pairs). The limit is
// the ptxas brx knee, which scales with the per-region BRANCHTARGET COUNT, not words: one region
// holding all ~15k Doom entries blew a ptxas child past 3 GB (watchdog-killed), while 2 regions of
// ~7.5k targets build at 1.38 GB. So the default is large, and rvx_codegen separately floors the
// region COUNT so no region ever exceeds ~8k brx targets.
#ifndef RVX_REGW
#define RVX_REGW 60000
#endif
static void rvx_codegen(std::vector<std::string>& units, std::vector<uint32_t>& pc2idx,
                        const uint32_t* img, int N, uint32_t base, const std::vector<uint8_t>& comp,
                        const std::vector<uint8_t>& disp, int nc, bool sptrust) {
    std::vector<uint32_t> body;
    for (int w=0; w<N; w++) if (comp[w]) body.push_back((uint32_t)w);
    const int nb = (int)body.size();
    int regw = RVX_REGW; if (const char* e=getenv("RVX_REGW")) { int v=atoi(e); if (v>0) regw=v; }
    if (g_rvx_regw_force > 0) regw = g_rvx_regw_force;      // assemble-failure retry (rvxblk_build halves it)
    if (nc > 1 && regw > 4000) regw = 4000;   // interleaved memops emit ~2× the PTX — keep the per-unit ptxas peak flat
    if (regw >= (1<<20)) regw = (1<<20)-1;                  // local ordinal must fit pc2idx bits 19:0
    int K = nb ? (nb + regw - 1) / regw : 1;
    // ptxas-knee guard: the assembler's working set scales with the per-region brx TARGET count
    // (≥ ~15k in one region blew a child past 3 GB). Floor the region count so each stays ≤ ~8k.
    { int nde = 0; for (int w=0; w<N; w++) nde += disp[w] ? 1 : 0;
      int Kt = (nde + 7999) / 8000; if (Kt > K) K = Kt; }
    if (K > 2048) K = 2048;                                 // region id fits 12 bits (no 0xFFFFFFFF alias)
    // Region cuts: start from equal-compiled-word-count ideals, then slide each cut (±700 body slots)
    // to the word boundary crossed by the FEWEST static edges (branch/jal/fall-through between compiled
    // words; backward edges ×8 — a loop crossing a cut pays the spill/refill EVERY iteration).
    std::vector<int> regof(N, -1);
    std::vector<int> cuts;                                  // body-index of each region's first word
    if (K > 1) {
        std::vector<long long> diff(N+1, 0);
        for (int w=0; w<N; w++) if (comp[w]) {
            uint32_t in=img[w], op=in&0x7F; uint32_t pc=base+(uint32_t)w*4;
            auto edge=[&](uint32_t t){ if(t<base||((t-base)&3)) return; int tw=(int)((t-base)>>2);
                if(tw<0||tw>=N||!comp[tw]||tw==w) return; int lo=w<tw?w:tw, hi=w<tw?tw:w, wt=tw<w?8:1;
                diff[lo+1]+=wt; diff[hi+1]-=wt; };
            if (op==0x63) edge(pc+rv_bimm(in));
            else if (op==0x6F) edge(pc+rv_jimm(in));
            bool term = (op==0x6F || op==0x67 || !rvx_compilable(op));
            if (!term && w+1<N && comp[w+1]) { diff[w+1]+=1; diff[w+2]-=1; }
        }
        std::vector<long long> cost(N+1, 0);                // cost[w] = edges crossing the boundary below word w
        { long long a=0; for (int w=0; w<=N; w++) { a+=diff[w]; cost[w]=a; } }
        for (int r=1; r<K; r++) {
            int ideal=(int)((long long)nb*r/K), lo=ideal-700, hi=ideal+700;
            int minlo = cuts.empty() ? 1 : cuts.back()+1;   // strictly increasing → every region nonempty
            int maxhi = nb-1 - (K-1-r);                     // leave room for the remaining cuts
            if (lo<minlo) lo=minlo; if (hi>maxhi) hi=maxhi;
            if (lo>hi) break;                               // no room left — settle for fewer regions
            int bi=lo; long long bc=0x7fffffffffffffffLL;
            for (int i=lo; i<=hi; i++) { long long c=cost[body[i]]; if (c<bc) { bc=c; bi=i; } }
            cuts.push_back(bi);
        }
    }
    K = (int)cuts.size() + 1;
    { int r=0; size_t ci=0;
      for (int i=0; i<nb; i++) { while (ci<cuts.size() && i>=cuts[ci]) { r++; ci++; } regof[body[i]]=r; } }
    // Region-entry words: disp leaders + every compiled word a CROSS-REGION static edge lands on.
    std::vector<uint8_t> xt(N, 0);
    for (int w=0; w<N; w++) if (comp[w]) {
        uint32_t in=img[w], op=in&0x7F; uint32_t pc=base+(uint32_t)w*4;
        auto mark=[&](uint32_t t){ if(t<base||((t-base)&3)) return; int tw=(int)((t-base)>>2);
            if (tw>=0 && tw<N && comp[tw] && regof[tw]!=regof[w]) xt[tw]=1; };
        if (op==0x63) mark(pc+rv_bimm(in));
        else if (op==0x6F) mark(pc+rv_jimm(in));
        bool term = (op==0x6F || op==0x67 || !rvx_compilable(op));
        if (!term && w+1<N && comp[w+1] && regof[w+1]!=regof[w]) xt[w+1]=1;
    }
    std::vector<std::vector<uint32_t>> rtgt(K);
    pc2idx.assign(N, 0xFFFFFFFFu);
    for (int w=0; w<N; w++) if (comp[w] && (disp[w] || xt[w])) {
        int r = regof[w];
        pc2idx[w] = ((uint32_t)r<<20) | (uint32_t)rtgt[r].size();   // ordinal < 2^20 (region ≤ ~regw words)
        rtgt[r].push_back((uint32_t)w);
    }
    for (int r=0; r<K; r++) if (rtgt[r].empty())            // never enterable, but keep the PTX well-formed
        for (int i=0; i<nb; i++) if (regof[body[i]]==r) { pc2idx[body[i]]=(uint32_t)r<<20; rtgt[r].push_back(body[i]); break; }
    // Possible-target bitmap: words enterable SIDEWAYS (branch/jal target anywhere in the image, or a
    // region-entry brx word). Cross-instruction CSE/alignment state is only valid along fall-through,
    // so it is reset at every such word. Scanning the whole image (incl. data words) is conservative-safe.
    std::vector<uint8_t> targ(N, 0);
    for (int w=0; w<N; w++) {
        uint32_t in = img[w], o = in&0x7F;
        if (pc2idx[w] != 0xFFFFFFFFu) targ[w] = 1;
        if (o!=0x63 && o!=0x6F) continue;
        uint32_t t = base + (uint32_t)w*4 + (o==0x63 ? rv_bimm(in) : rv_jimm(in));
        if (t>=base && ((t-base)&3)==0 && ((t-base)>>2) < (uint32_t)N) targ[(t-base)>>2] = 1;
    }
    // DIVLOOP/COPYLOOP pre-scan: the replacements jump to the loop exit with different scratch/cache
    // state than the per-instruction path, so the exits must be reset points.
    std::vector<uint8_t> divhit(N, 0), cpyhit(N, 0), mulhit(N, 0), fillhit(N, 0), palhit(N, 0);
    int ncpy = 0, nfill = 0, npal = 0;
    { RvxDiv dr; RvxCpy cr; RvxMul mr; RvxFill fr; RvxPal pr;
      for (int w=0; w+13 < N; w++)
          if (comp[w] && rvx_match_divloop(img, N, base, w, dr)) { divhit[w] = 1; targ[w+13] = 1; }
      for (int w=0; w+7 < N; w++)
          if (comp[w] && rvx_match_mulloop(img, N, base, w, mr)) { mulhit[w] = 1; targ[w+7] = 1; }
      if (nc == 1) {
          for (int w=0; w+5 < N; w++)
              if (comp[w] && rvx_match_copyloop(img, N, base, w, cr)) { cpyhit[w] = 1; targ[w+5] = 1; ncpy++; }
          for (int w=0; w+3 < N; w++)
              if (comp[w] && rvx_match_fillloop(img, N, base, w, fr)) { fillhit[w] = (uint8_t)fr.nw; targ[w+fr.nw] = 1; nfill++; }
          for (int w=0; w+14 < N; w++)
              if (comp[w] && rvx_match_palexp(img, N, base, w, pr)) { palhit[w] = 1; targ[w+14] = 1; npal++; }
      } }
    if (getenv("RVX_STATS") && ncpy) fprintf(stderr, "[xblk] copyloop sites: %d\n", ncpy);
    if (getenv("RVX_STATS") && nfill) fprintf(stderr, "[xblk] fillloop sites: %d\n", nfill);
    if (getenv("RVX_STATS") && npal) fprintf(stderr, "[xblk] palexp sites: %d\n", npal);

    const char* hdr = ".version 7.8\n.target sm_86\n.address_size 64\n";
    units.assign(1+K + (ncpy ? 1 : 0) + (nfill ? 1 : 0) + (npal ? 1 : 0), std::string());
    // ── xcopy unit (knee-ISOLATED: assembles alone in microseconds). Word-widened forward byte copy:
    //    byte head until DST is 4-aligned, then a funnel-shift body (the +8 B buffer guard covers the
    //    high word of an unaligned source), then a byte tail. Callers guarantee n ≥ 1 and NO forward
    //    overlap (D-S ≥ n unsigned) — overlapping copies keep the original byte-propagation loop.
    if (ncpy) {
        std::string& xc = units[1+K];
        xc  = hdr;
        xc += ".visible .func xcopy (.param .b64 ps, .param .b64 pd, .param .b32 pn)\n{\n"
              ".reg .b64 %s,%d,%b,%bo;\n.reg .b32 %n,%v,%w0,%w1,%sh,%t;\n.reg .pred %q;\n"
              "ld.param.u64 %s,[ps];\nld.param.u64 %d,[pd];\nld.param.u32 %n,[pn];\n"
              "CH:\ncvt.u32.u64 %t, %d;\nand.b32 %t, %t, 3;\nsetp.eq.u32 %q, %t, 0;\n@%q bra CB;\n"
              "setp.eq.u32 %q, %n, 0;\n@%q bra CE;\n"
              "ld.global.u8 %v, [%s];\nst.global.u8 [%d], %v;\n"
              "add.u64 %s, %s, 1;\nadd.u64 %d, %d, 1;\nsub.u32 %n, %n, 1;\nbra CH;\n"
              "CB:\nsetp.lt.u32 %q, %n, 4;\n@%q bra CT;\n"
              "cvt.u32.u64 %t, %s;\nand.b32 %t, %t, 3;\nshl.b32 %sh, %t, 3;\n"
              "cvt.u64.u32 %bo, %t;\nsub.u64 %b, %s, %bo;\n"
              "CL:\nld.global.u32 %w0, [%b];\nld.global.u32 %w1, [%b+4];\n"
              "shf.r.wrap.b32 %v, %w0, %w1, %sh;\nst.global.u32 [%d], %v;\n"
              "add.u64 %b, %b, 4;\nadd.u64 %d, %d, 4;\nadd.u64 %s, %s, 4;\nsub.u32 %n, %n, 4;\n"
              "setp.ge.u32 %q, %n, 4;\n@%q bra CL;\n"
              "CT:\nsetp.eq.u32 %q, %n, 0;\n@%q bra CE;\n"
              "ld.global.u8 %v, [%s];\nst.global.u8 [%d], %v;\n"
              "add.u64 %s, %s, 1;\nadd.u64 %d, %d, 1;\nsub.u32 %n, %n, 1;\nbra CT;\n"
              "CE:\nret;\n}\n";
    }
    // ── xfill unit (knee-ISOLATED, same playbook). Fills n bytes at d with the 32-bit pattern v,
    //    which the call site pre-replicates to word width (so byte (a&3) of v is the right byte for
    //    ANY address a — callers guarantee d ≡ 0 mod the element width, and width divides 4): byte
    //    head until d is 4-aligned, word body, byte tail.
    if (nfill) {
        std::string& xf = units[1+K + (ncpy ? 1 : 0)];
        xf  = hdr;
        xf += ".visible .func xfill (.param .b64 pd, .param .b32 pv, .param .b32 pn)\n{\n"
              ".reg .b64 %d;\n.reg .b32 %n,%v,%t,%sh;\n.reg .pred %q;\n"
              "ld.param.u64 %d,[pd];\nld.param.u32 %v,[pv];\nld.param.u32 %n,[pn];\n"
              "FH:\nsetp.eq.u32 %q, %n, 0;\n@%q bra FE;\n"
              "cvt.u32.u64 %t, %d;\nand.b32 %t, %t, 3;\nsetp.eq.u32 %q, %t, 0;\n@%q bra FB;\n"
              "shl.b32 %sh, %t, 3;\nshr.u32 %t, %v, %sh;\nst.global.u8 [%d], %t;\n"
              "add.u64 %d, %d, 1;\nsub.u32 %n, %n, 1;\nbra FH;\n"
              "FB:\nsetp.lt.u32 %q, %n, 4;\n@%q bra FT;\n"
              "FL:\nst.global.u32 [%d], %v;\nadd.u64 %d, %d, 4;\nsub.u32 %n, %n, 4;\n"
              "setp.ge.u32 %q, %n, 4;\n@%q bra FL;\n"
              "FT:\nsetp.eq.u32 %q, %n, 0;\n@%q bra FE;\n"
              "cvt.u32.u64 %t, %d;\nand.b32 %t, %t, 3;\nshl.b32 %sh, %t, 3;\n"
              "shr.u32 %t, %v, %sh;\nst.global.u8 [%d], %t;\n"
              "add.u64 %d, %d, 1;\nsub.u32 %n, %n, 1;\nbra FT;\n"
              "FE:\nret;\n}\n";
    }
    // ── xpal unit (knee-ISOLATED — the r11/r13 inline emissions of this loop tripped the ptxas
    //    knee; the dedicated unit assembles alone). Per pixel: idx = src[i]; r,g,b = pal[idx*3..];
    //    ONE word store r|g<<8|b<<16|va (va = alpha<<24, precomputed by the call site, which also
    //    guarantees d word-aligned, n ≥ 1, and dst disjoint from src and palette).
    if (npal) {
        std::string& xp = units[1+K + (ncpy ? 1 : 0) + (nfill ? 1 : 0)];
        xp  = hdr;
        xp += ".visible .func xpal (.param .b64 ps, .param .b64 pd, .param .b64 pp, .param .b32 pn, .param .b32 pa)\n{\n"
              ".reg .b64 %s,%d,%pl,%pe;\n.reg .b32 %n,%i,%r,%g,%b,%w,%va;\n.reg .pred %q;\n"
              "ld.param.u64 %s,[ps];\nld.param.u64 %d,[pd];\nld.param.u64 %pl,[pp];\n"
              "ld.param.u32 %n,[pn];\nld.param.u32 %va,[pa];\n"
              "PX:\nld.global.u8 %i, [%s];\nadd.u64 %s, %s, 1;\n"
              "mul.lo.u32 %i, %i, 3;\ncvt.u64.u32 %pe, %i;\nadd.u64 %pe, %pe, %pl;\n"
              "ld.global.u8 %r, [%pe];\nld.global.u8 %g, [%pe+1];\nld.global.u8 %b, [%pe+2];\n"
              "shl.b32 %g, %g, 8;\nshl.b32 %b, %b, 16;\nor.b32 %w, %r, %g;\n"
              "or.b32 %w, %w, %b;\nor.b32 %w, %w, %va;\nst.global.u32 [%d], %w;\n"
              "add.u64 %d, %d, 4;\nsub.u32 %n, %n, 1;\nsetp.ne.u32 %q, %n, 0;\n@%q bra PX;\n"
              "ret;\n}\n";
    }
    // ── region units. Prologue: load regs/cnt/budget/M/P2I + the entry ordinal from XS, brx to it.
    //    XDISP: jalr lands here — budget gate → bounds → pc2idx → same region ⇒ re-brx, else XSAVE.
    //    XSAVE: spill regs/pc/cnt to XS, ret (the dispatcher decides re-enter vs exit).
    int bi = 0;                                             // walking index into body[]
    for (int r=0; r<K; r++) {
        std::string& ptx = units[1+r];
        ptx  = hdr;
        ptx += ".extern .shared .align 8 .b8 XS[];\n";   // dynamic shared (168 B × blockDim at launch) — per-CTA, far cheaper than .global per region transition
        if (ncpy) ptx += ".extern .func xcopy (.param .b64 ps, .param .b64 pd, .param .b32 pn);\n";
        if (nfill) ptx += ".extern .func xfill (.param .b64 pd, .param .b32 pv, .param .b32 pn);\n";
        if (npal) ptx += ".extern .func xpal (.param .b64 ps, .param .b64 pd, .param .b64 pp, .param .b32 pn, .param .b32 pa);\n";
        rvx_app(ptx, ".visible .func xr%d\n{\n", r);
        ptx += ".reg .b64 %M,%P2I,%a0,%a1,%ab,%ad,%xs,%ss;\n.reg .b32 %x<32>,%t0,%t1,%t2,%t3,%pc,%budget,%cnt;\n"
               ".reg .u32 %wi,%bidx,%rg,%tx,%rsp;\n.reg .pred %p0,%p1,%p2;\n";
        ptx += "mov.u64 %xs, XS;\nmov.u32 %tx, %tid.x;\nmul.wide.u32 %ad, %tx, 168;\nadd.u64 %xs, %xs, %ad;\n";
        // Shadow return stack base: after all XS slots — %ss = XS + 168*ntid + 128*tid (16 × 8 B / thread).
        ptx += "mov.u64 %ss, XS;\nmov.u32 %wi, %ntid.x;\nmul.wide.u32 %ad, %wi, 168;\nadd.u64 %ss, %ss, %ad;\n"
               "mul.wide.u32 %ad, %tx, 128;\nadd.u64 %ss, %ss, %ad;\n";
        ptx += "mov.b32 %x0, 0;\n";
        for (int g=1; g<32; g++) rvx_app(ptx, "ld.shared.u32 %%x%u, [%%xs+%d];\n", g, g*4);
        ptx += "ld.shared.u32 %cnt, [%xs+132];\nld.shared.u32 %budget, [%xs+136];\n"
               "ld.shared.u64 %M, [%xs+144];\nld.shared.u64 %P2I, [%xs+152];\nld.shared.u32 %bidx, [%xs+160];\n"
               "ld.shared.u32 %rsp, [%xs+164];\n";
        ptx += "BT: .branchtargets ";
        for (size_t i=0; i<rtgt[r].size(); i++) rvx_app(ptx, "%sL%u", i?",":"", base+rtgt[r][i]*4);
        ptx += ";\nJBRX:\nbrx.idx %bidx, BT;\n";            // the ONLY brx in this unit (XDISP reuses it)
        RvxCse cse; cse.spInit = sptrust ? 0 : 0xFF; cse.reset();
        std::string cold;                               // misaligned-store fallbacks, emitted after the body
        int prev = -2;
        for (; bi<nb && regof[body[bi]]==r; bi++) {
            uint32_t w = body[bi], pc = base + w*4, op = img[w]&0x7F;
            if ((int)w != prev+1 || targ[w]) cse.reset();   // run start or sideways-enterable
            // Software-divide loop → ONE hardware div+rem. The precondition test (canonical entry
            // state R==0, Q==0, i==31 — what the function prologue establishes) routes mid-loop
            // sideways re-entries to the original code below; entering the header IN that state is
            // the canonical entry regardless of how control got there, so the closed form is exact:
            // Q=N/D, R=N%D, i=-1 (PTX div/rem by zero yield 0xFFFFFFFF and the dividend — exactly
            // the soft loop's results), 13×32=416 retired atomically (overshoot is legal). The loop
            // temps T1/T2 keep their pre-loop values — same treatment as the interpreter's
            // RC_DIVLOOP arm (they are dead clang temps; the bit-identical gate covers it).
            if (divhit[w] && (int)w+13 < N && regof[w+13]==r && comp[w+13]) {
                RvxDiv D0; rvx_match_divloop(img, N, base, (int)w, D0);
                rvx_app(ptx, "L%u:\n", pc);
                rvx_app(ptx, "or.b32 %%t0, %%x%u, %%x%u;\nxor.b32 %%t1, %%x%u, 31;\nor.b32 %%t0, %%t0, %%t1;\n"
                             "setp.ne.u32 %%p0, %%t0, 0;\n@%%p0 bra DORIG%u;\n", D0.R, D0.Q, D0.I, pc);
                rvx_app(ptx, "add.s32 %%cnt, %%cnt, 416;\n"
                             "div.u32 %%t0, %%x%u, %%x%u;\nrem.u32 %%t1, %%x%u, %%x%u;\n",
                             D0.N, D0.D, D0.N, D0.D);
                rvx_app(ptx, "mov.b32 %%x%u, %%t0;\nmov.b32 %%x%u, %%t1;\nmov.b32 %%x%u, -1;\n",
                             D0.Q, D0.R, D0.I);
                rvx_app(ptx, "bra L%u;\n", base+(w+13)*4);
                rvx_app(ptx, "DORIG%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
                cse.reset();                                  // the test clobbered scratch state
            }
            // Software-multiply loop → ONE mul.lo, straight-line, valid from ANY entry state (the
            // loop's effect from the header is ACC += B·M over current values). Exact final state
            // and exact data-dependent retire: 7/iter, it = M ? 32-clz(M) : 1 (do-while ≥1).
            else if (mulhit[w] && (int)w+7 < N && regof[w+7]==r && comp[w+7]) {
                RvxMul M0; rvx_match_mulloop(img, N, base, (int)w, M0);
                rvx_app(ptx, "L%u:\n", pc);
                rvx_app(ptx, "clz.b32 %%t0, %%x%u;\nsub.u32 %%t0, 32, %%t0;\n"
                             "setp.eq.u32 %%p0, %%x%u, 0;\n@%%p0 mov.b32 %%t0, 1;\n", M0.M, M0.M);
                ptx += "mul.lo.u32 %t1, %t0, 7;\nadd.s32 %cnt, %cnt, %t1;\n";
                rvx_app(ptx, "mul.lo.u32 %%t1, %%x%u, %%x%u;\nadd.u32 %%x%u, %%x%u, %%t1;\n",
                             M0.B, M0.M, M0.ACC, M0.ACC);
                rvx_app(ptx, "sub.u32 %%t1, %%t0, 1;\nshl.b32 %%t1, %%x%u, %%t1;\n@%%p0 mov.b32 %%t1, 0;\n", M0.B);
                rvx_app(ptx, "shl.b32 %%t2, %%x%u, %%t0;\n", M0.B);
                rvx_app(ptx, "mov.b32 %%x%u, %%t1;\nmov.b32 %%x%u, %%t2;\nmov.b32 %%x%u, 0;\n",
                             M0.A, M0.B, M0.M);
                rvx_app(ptx, "bra L%u;\n", base+(w+7)*4);
                rvx_app(ptx, "MORIG%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
                cse.reset();
            }
            // Byte-copy loop → xcopy call (word-widened). Guards route to the kept original loop:
            // len==0 (the do-while would wrap 2^32 — pathological, keep original semantics), forward
            // OVERLAP (D-S < len: the byte loop PROPAGATES bytes, a block copy would not), and wild
            // lengths (>16 M: bound the atomic retire). Exact closed form after the call: retire
            // 5·len, RT = last byte (re-read from src — the no-overlap guard makes it unclobbered),
            // S += len, D = END.
            else if (cpyhit[w] && (int)w+5 < N && regof[w+5]==r && comp[w+5]) {
                RvxCpy C0; rvx_match_copyloop(img, N, base, (int)w, C0);
                rvx_app(ptx, "L%u:\n", pc);
                rvx_app(ptx, "sub.u32 %%t0, %%x%u, %%x%u;\n"                      // len = END - D
                             "setp.eq.u32 %%p0, %%t0, 0;\n"
                             "setp.gt.u32 %%p1, %%t0, 16777216;\nor.pred %%p0, %%p0, %%p1;\n"
                             "sub.u32 %%t1, %%x%u, %%x%u;\n"                      // D - S (forward overlap test)
                             "setp.lt.u32 %%p1, %%t1, %%t0;\nor.pred %%p0, %%p0, %%p1;\n"
                             "@%%p0 bra CPORIG%u;\n",
                             C0.END, C0.D, C0.D, C0.S, pc);
                rvx_app(ptx, "mul.lo.u32 %%t1, %%t0, 5;\nadd.s32 %%cnt, %%cnt, %%t1;\n");
                rvx_app(ptx, "cvt.u64.u32 %%a0, %%x%u;\nadd.u64 %%a0, %%a0, %%M;\n"
                             "cvt.u64.u32 %%a1, %%x%u;\nadd.u64 %%a1, %%a1, %%M;\n", C0.S, C0.D);
                ptx += "{ .param .b64 ps; .param .b64 pd; .param .b32 pn;\n"
                       "st.param.b64 [ps], %a0;\nst.param.b64 [pd], %a1;\nst.param.b32 [pn], %t0;\n"
                       "call.uni xcopy, (ps, pd, pn);\n}\n";
                rvx_app(ptx, "add.u32 %%t1, %%x%u, %%t0;\nsub.u32 %%t1, %%t1, 1;\n"
                             "cvt.u64.u32 %%a0, %%t1;\nadd.u64 %%a0, %%a0, %%M;\n"
                             "ld.global.%s %%x%u, [%%a0];\n", C0.S, C0.sext ? "s8" : "u8", C0.RT);
                rvx_app(ptx, "add.u32 %%x%u, %%x%u, %%t0;\nmov.b32 %%x%u, %%x%u;\n",
                             C0.S, C0.S, C0.D, C0.END);
                rvx_app(ptx, "bra L%u;\n", base+(w+5)*4);
                rvx_app(ptx, "CPORIG%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
                cse.reset();
            }
            // Fill loop → xfill call (pattern pre-replicated to a 32-bit word). Guards route to the
            // kept original loop: len==0 (the do-while would wrap 2^32), wild lengths (>16 M, bound
            // the atomic retire), and for form B widths >1: len not a multiple of the element width
            // (the guest loop would never terminate) or D misaligned vs the width (the byte-phase
            // replication trick needs D ≡ 0 mod width). Exact closed form after the call: retire
            // 4·len (form A) or 3·(len>>f3) (form B), D = END (form A also nD = END); VAL/END are
            // loop-invariant.
            else if (fillhit[w] && (int)w+fillhit[w] < N && regof[w+fillhit[w]]==r && comp[w+fillhit[w]]) {
                RvxFill F0; rvx_match_fillloop(img, N, base, (int)w, F0);
                rvx_app(ptx, "L%u:\n", pc);
                rvx_app(ptx, "sub.u32 %%t0, %%x%u, %%x%u;\n"                      // len = END - D
                             "setp.eq.u32 %%p0, %%t0, 0;\n"
                             "setp.gt.u32 %%p1, %%t0, 16777216;\nor.pred %%p0, %%p0, %%p1;\n",
                             F0.END, F0.D);
                if (F0.f3)
                    rvx_app(ptx, "or.b32 %%t1, %%x%u, %%t0;\nand.b32 %%t1, %%t1, %u;\n"
                                 "setp.ne.u32 %%p1, %%t1, 0;\nor.pred %%p0, %%p0, %%p1;\n",
                                 F0.D, (1u<<F0.f3)-1u);
                rvx_app(ptx, "@%%p0 bra FIORIG%u;\n", pc);
                if (F0.nw==4)        ptx += "shl.b32 %t1, %t0, 2;\nadd.s32 %cnt, %cnt, %t1;\n";
                else if (F0.f3==0)   ptx += "mul.lo.u32 %t1, %t0, 3;\nadd.s32 %cnt, %cnt, %t1;\n";
                else { rvx_app(ptx, "shr.u32 %%t1, %%t0, %u;\n", F0.f3);
                       ptx += "mul.lo.u32 %t1, %t1, 3;\nadd.s32 %cnt, %cnt, %t1;\n"; }
                if (F0.f3==0)        rvx_app(ptx, "and.b32 %%t2, %%x%u, 255;\nmul.lo.u32 %%t2, %%t2, 16843009;\n", F0.VAL);
                else if (F0.f3==1)   rvx_app(ptx, "and.b32 %%t2, %%x%u, 65535;\nmul.lo.u32 %%t2, %%t2, 65537;\n", F0.VAL);
                else                 rvx_app(ptx, "mov.b32 %%t2, %%x%u;\n", F0.VAL);
                rvx_app(ptx, "cvt.u64.u32 %%a0, %%x%u;\nadd.u64 %%a0, %%a0, %%M;\n", F0.D);
                ptx += "{ .param .b64 pd; .param .b32 pv; .param .b32 pn;\n"
                       "st.param.b64 [pd], %a0;\nst.param.b32 [pv], %t2;\nst.param.b32 [pn], %t0;\n"
                       "call.uni xfill, (pd, pv, pn);\n}\n";
                rvx_app(ptx, "mov.b32 %%x%u, %%x%u;\n", F0.D, F0.END);
                if (F0.ND) rvx_app(ptx, "mov.b32 %%x%u, %%x%u;\n", F0.ND, F0.END);
                rvx_app(ptx, "bra L%u;\n", base+(w+(uint32_t)F0.nw)*4);
                rvx_app(ptx, "FIORIG%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
                cse.reset();
            }
            // Palette-expand loop → xpal call (one word store per pixel). Guards route to the kept
            // original loop: n==0 (the do-while would wrap 2^32), n>16M (bound the atomic retire),
            // DST-3 not word-aligned (the unit stores words), and dst overlapping src or the
            // palette (the unit re-reads pal per pixel and the closed form re-reads the last pixel
            // — disjointness makes both exact). Closed form: retire 14·n, SRC=END, DST+=4n, temps
            // IDX/T/P/C0/C1/C2 re-derived from the last pixel in guest write order (all 11 regs
            // pairwise distinct per the matcher, so no aliasing).
            else if (palhit[w] && (int)w+14 < N && regof[w+14]==r && comp[w+14]) {
                RvxPal P0; rvx_match_palexp(img, N, base, (int)w, P0);
                rvx_app(ptx, "L%u:\n", pc);
                rvx_app(ptx, "sub.u32 %%t0, %%x%u, %%x%u;\n"                      // n = END - SRC
                             "setp.eq.u32 %%p0, %%t0, 0;\n"
                             "setp.gt.u32 %%p1, %%t0, 16777216;\nor.pred %%p0, %%p0, %%p1;\n",
                             P0.END, P0.SRC);
                rvx_app(ptx, "sub.u32 %%t1, %%x%u, 3;\n"                          // t1 = dst word base
                             "and.b32 %%t2, %%t1, 3;\nsetp.ne.u32 %%p1, %%t2, 0;\nor.pred %%p0, %%p0, %%p1;\n",
                             P0.DST);
                ptx += "shl.b32 %t2, %t0, 2;\n";                                  // t2 = 4n
                rvx_app(ptx, "add.u32 %%t3, %%x%u, %%t0;\nsetp.lt.u32 %%p1, %%t1, %%t3;\n"   // dst ∩ src
                             "add.u32 %%t3, %%t1, %%t2;\nsetp.lt.u32 %%p2, %%x%u, %%t3;\n"
                             "and.pred %%p1, %%p1, %%p2;\nor.pred %%p0, %%p0, %%p1;\n",
                             P0.SRC, P0.SRC);
                rvx_app(ptx, "add.u32 %%t3, %%x%u, 768;\nsetp.lt.u32 %%p1, %%t1, %%t3;\n"    // dst ∩ pal
                             "add.u32 %%t3, %%t1, %%t2;\nsetp.lt.u32 %%p2, %%x%u, %%t3;\n"
                             "and.pred %%p1, %%p1, %%p2;\nor.pred %%p0, %%p0, %%p1;\n",
                             P0.PAL, P0.PAL);
                rvx_app(ptx, "@%%p0 bra PXORIG%u;\n", pc);
                ptx += "mul.lo.u32 %t3, %t0, 14;\nadd.s32 %cnt, %cnt, %t3;\n";
                rvx_app(ptx, "cvt.u64.u32 %%a0, %%x%u;\nadd.u64 %%a0, %%a0, %%M;\n"   // src
                             "cvt.u64.u32 %%a1, %%t1;\nadd.u64 %%a1, %%a1, %%M;\n"    // dst word base
                             "cvt.u64.u32 %%ad, %%x%u;\nadd.u64 %%ad, %%ad, %%M;\n"   // palette
                             "shl.b32 %%t3, %%x%u, 24;\n",                            // alpha<<24
                             P0.SRC, P0.PAL, P0.ALPHA);
                ptx += "{ .param .b64 ps; .param .b64 pd; .param .b64 pp; .param .b32 pn; .param .b32 pa;\n"
                       "st.param.b64 [ps], %a0;\nst.param.b64 [pd], %a1;\nst.param.b64 [pp], %ad;\n"
                       "st.param.b32 [pn], %t0;\nst.param.b32 [pa], %t3;\n"
                       "call.uni xpal, (ps, pd, pp, pn, pa);\n}\n";
                rvx_app(ptx, "sub.u32 %%t3, %%x%u, 1;\n"                          // last pixel idx
                             "cvt.u64.u32 %%a0, %%t3;\nadd.u64 %%a0, %%a0, %%M;\n"
                             "ld.global.u8 %%x%u, [%%a0];\n", P0.END, P0.IDX);
                rvx_app(ptx, "shl.b32 %%x%u, %%x%u, 1;\n", P0.T, P0.IDX);
                rvx_app(ptx, "add.u32 %%x%u, %%x%u, %%x%u;\nadd.u32 %%x%u, %%x%u, %%x%u;\n",
                             P0.P, P0.PAL, P0.IDX, P0.P, P0.P, P0.T);
                rvx_app(ptx, "cvt.u64.u32 %%a0, %%x%u;\nadd.u64 %%a0, %%a0, %%M;\n"
                             "ld.global.u8 %%x%u, [%%a0];\nld.global.u8 %%x%u, [%%a0+1];\nld.global.u8 %%x%u, [%%a0+2];\n",
                             P0.P, P0.C0, P0.C1, P0.C2);
                rvx_app(ptx, "add.u32 %%x%u, %%t1, %%t2;\nadd.u32 %%x%u, %%x%u, 3;\n"   // DST = base+4n+3
                             "mov.b32 %%x%u, %%x%u;\n", P0.DST, P0.DST, P0.DST, P0.SRC, P0.END);
                rvx_app(ptx, "bra L%u;\n", base+(w+14)*4);
                rvx_app(ptx, "PXORIG%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
                cse.reset();
            }
            else rvx_app(ptx, "L%u:\nadd.s32 %%cnt, %%cnt, 1;\n", pc);
            rvx_emit(ptx, pc, img[w], comp, comp, N, base, regof, r, cse, nc, pc2idx, cold);
            bool terminates = (op==0x6F || op==0x67 || !rvx_compilable(op));   // jal/jalr/system already transfer
            if (!terminates) {                              // fall-through: redirect if w+1 leaves the region
                int nw = (int)w + 1;
                if (nw>=N || !comp[nw] || regof[nw]!=r) rvx_app(ptx,"mov.u32 %%pc, %u;\nbra XSAVE;\n", base+(uint32_t)nw*4);
            }
            else cse.reset();                               // nothing falls through a terminator
            prev = (int)w;
        }
        ptx += cold;                                    // cold misaligned-store blocks (each ends with bra CJ<pc>)
        ptx += "XDISP:\nsetp.ge.s32 %p0, %cnt, %budget;\n@%p0 bra XSAVE;\n";
        rvx_app(ptx, "sub.u32 %%wi, %%pc, %u;\nshr.u32 %%wi, %%wi, 2;\n", base);
        rvx_app(ptx, "setp.ge.u32 %%p0, %%wi, %u;\n@%%p0 bra XSAVE;\n", (uint32_t)N);
        ptx += "mul.wide.u32 %ad, %wi, 4;\nadd.u64 %ad, %ad, %P2I;\nld.global.u32 %bidx, [%ad];\n"
               "setp.eq.u32 %p0, %bidx, 4294967295;\n@%p0 bra XSAVE;\n";
        rvx_app(ptx, "shr.u32 %%rg, %%bidx, 20;\nsetp.ne.u32 %%p0, %%rg, %d;\n@%%p0 bra XSAVE;\n", r);
        ptx += "and.b32 %bidx, %bidx, 1048575;\nbra JBRX;\n";
        ptx += "XSAVE:\n";
        for (int g=1; g<32; g++) rvx_app(ptx, "st.shared.u32 [%%xs+%d], %%x%u;\n", g*4, g);
        ptx += "st.shared.u32 [%xs+128], %pc;\nst.shared.u32 [%xs+132], %cnt;\nst.shared.u32 [%xs+164], %rsp;\nret;\n}\n";
    }
    // ── dispatcher unit: state→XS, then loop { budget/bounds/pc2idx gate → call region } → state←XS.
    std::string& ptx = units[0];
    ptx  = hdr;
    ptx += ".extern .shared .align 8 .b8 XS[];\n";       // same dynamic-shared segment the region units alias
    for (int r=0; r<K; r++) rvx_app(ptx, ".extern .func xr%d;\n", r);
    ptx += ".visible .entry xk(.param .u64 pM,.param .u64 pS,.param .u32 pBud,.param .u64 pP2I,.param .u64 pRet){\n";
    ptx += ".reg .b64 %M,%S,%P2I,%RET,%ad,%xs;\n.reg .b32 %t0,%pc,%budget,%cnt;\n.reg .u32 %wi,%bidx,%rg,%gid,%tx;\n.reg .pred %p0,%pz;\n";
    ptx += "ld.param.u64 %M,[pM];\nld.param.u64 %S,[pS];\nld.param.u32 %budget,[pBud];\n"
           "ld.param.u64 %P2I,[pP2I];\nld.param.u64 %RET,[pRet];\n";
    // One thread per guest core: gid-guard, per-thread XS slot (168 B), per-core CoreState (%S +=
    // gid*132) and interleave lane base (%M += 4*gid — the regions' address math then only needs
    // the (a&~3)*nc word term). Core 0 alone reports the retired count.
    ptx += "mov.u32 %tx, %tid.x;\nmov.u32 %rg, %ctaid.x;\nmov.u32 %wi, %ntid.x;\nmad.lo.u32 %gid, %rg, %wi, %tx;\n";
    rvx_app(ptx, "setp.ge.u32 %%pz, %%gid, %d;\n@%%pz ret;\n", nc);
    ptx += "mov.u64 %xs, XS;\nmul.wide.u32 %ad, %tx, 168;\nadd.u64 %xs, %xs, %ad;\n";
    ptx += "mul.wide.u32 %ad, %gid, 132;\nadd.u64 %S, %S, %ad;\n";
    if (nc > 1) ptx += "mul.wide.u32 %ad, %gid, 4;\nadd.u64 %M, %M, %ad;\n";
    ptx += "setp.eq.u32 %pz, %gid, 0;\n";
    for (int g=1; g<32; g++) rvx_app(ptx, "ld.global.u32 %%t0, [%%S+%d];\nst.shared.u32 [%%xs+%d], %%t0;\n", g*4, g*4);
    rvx_app(ptx, "ld.global.u32 %%pc, [%%S+%d];\nmov.b32 %%cnt, 0;\n", 32*4);
    ptx += "st.shared.u32 [%xs+132], %cnt;\nst.shared.u32 [%xs+136], %budget;\n"
           "st.shared.u64 [%xs+144], %M;\nst.shared.u64 [%xs+152], %P2I;\n"
           "st.shared.u32 [%xs+164], %cnt;\n";   // shadow-stack pointer = 0 (cold stack: rets miss to XDISP)
    ptx += "DLOOP:\nsetp.ge.s32 %p0, %cnt, %budget;\n@%p0 bra XSAVE;\n";
    rvx_app(ptx, "sub.u32 %%wi, %%pc, %u;\nshr.u32 %%wi, %%wi, 2;\n", base);
    rvx_app(ptx, "setp.ge.u32 %%p0, %%wi, %u;\n@%%p0 bra XSAVE;\n", (uint32_t)N);
    ptx += "mul.wide.u32 %ad, %wi, 4;\nadd.u64 %ad, %ad, %P2I;\nld.global.u32 %bidx, [%ad];\n";
    ptx += "setp.eq.u32 %p0, %bidx, 4294967295;\n@%p0 bra XSAVE;\n";
    ptx += "shr.u32 %rg, %bidx, 20;\nand.b32 %bidx, %bidx, 1048575;\nst.shared.u32 [%xs+160], %bidx;\n";
    for (int r=0; r<K; r++) rvx_app(ptx, "setp.eq.u32 %%p0, %%rg, %d;\n@%%p0 call.uni xr%d;\n", r, r);
    ptx += "ld.shared.u32 %pc, [%xs+128];\nld.shared.u32 %cnt, [%xs+132];\nbra DLOOP;\n";
    // save architectural state; core 0 alone returns the retired count
    ptx += "XSAVE:\n";
    for (int g=1; g<32; g++) rvx_app(ptx, "ld.shared.u32 %%t0, [%%xs+%d];\nst.global.u32 [%%S+%d], %%t0;\n", g*4, g*4);
    rvx_app(ptx,"st.global.u32 [%%S+%d], %%pc;\n@%%pz st.global.u32 [%%RET], %%cnt;\nret;\n}\n", 32*4);
}

// Compile each PTX unit OUT-OF-PROCESS with the toolkit's ptxas (-c --disable-optimizer-constants),
// then device-link the cubins into ONE module; fetch entry `xk`. Two reasons ptxas must run standalone:
//   1. the per-entry "compiler-generated constants" bank overflows when the linker folds K brx tables
//      into xk's budget — only the --disable-optimizer-constants flag avoids it, and the driver JIT has
//      no way to pass it;
//   2. ptxas' multi-GB brx working set then lives (and dies) in a child process, not ours.
// If ptxas.exe can't be found/run, falls back to in-driver PTX compilation (fine for small programs).
// RVX_STATS=1 prints the JIT/link info log. Returns 0 on success.
// RVX_PTXAS_O overrides the ptxas opt level (default 3). Lower levels assemble several times faster;
// runtime quality must be A/B'd (r7/r8 found -O3's extra passes win little on this machine-generated
// PTX). The level is part of the cubin disk-cache key.
static int rvx_ptxas_olvl() {
    static int s = -1;
    if (s < 0) { const char* o = getenv("RVX_PTXAS_O"); s = (o && *o >= '0' && *o <= '3') ? (*o - '0') : 3; }
    return s;
}
static int rvx_assemble_unit(const std::string& ptx, std::vector<char>& cubin) {
#ifdef _WIN32
    static std::atomic<int> s_seq{0};
    int seq = s_seq.fetch_add(1);
    char dir[MAX_PATH]; if (!GetTempPathA(sizeof dir, dir)) return -1;
    char fin[MAX_PATH], fout[MAX_PATH];
    snprintf(fin,  sizeof fin,  "%sxblk_%lu_%d.ptx",   dir, GetCurrentProcessId(), seq);
    snprintf(fout, sizeof fout, "%sxblk_%lu_%d.cubin", dir, GetCurrentProcessId(), seq);
    FILE* f = fopen(fin, "wb"); if (!f) return -1;
    fwrite(ptx.data(), 1, ptx.size(), f); fclose(f);
    const char* cp = getenv("CUDA_PATH");
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "\"%s%sptxas.exe\" -arch=sm_86 -O%d -c --disable-optimizer-constants \"%s\" -o \"%s\"",
             cp?cp:"", cp?"\\bin\\":"", rvx_ptxas_olvl(), fin, fout);
    STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    int rc = -1;
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        // WATCHDOG: ptxas sits on a knife edge for large brx-heavy units — pathological inputs have
        // been observed to spin for minutes at multi-GB commits. Hard limits (2.5 GB private bytes /
        // 120 s wall) kill the child and report failure; the caller then retries with smaller regions.
        DWORD waited = 0; bool wkilled = false;
        while (WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT) {
            waited += 250;
            PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof pmc;
            bool over = GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof pmc) &&
                        pmc.PagefileUsage > (SIZE_T)2560u * 1024u * 1024u;
            if (over || waited > 120000) {
                TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000);
                wkilled = true;
                fprintf(stderr, "[xblk] ptxas watchdog: killed child (%s after %lu ms, %.2f GB)\n",
                        over ? "memory" : "timeout", (unsigned long)waited, pmc.PagefileUsage / 1073741824.0);
                break;
            }
        }
        DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (!wkilled && ec == 0 && (f = fopen(fout, "rb")) != nullptr) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            cubin.resize((size_t)sz);
            rc = (fread(cubin.data(), 1, (size_t)sz, f) == (size_t)sz) ? 0 : -1;
            fclose(f);
        }
    }
    remove(fin); remove(fout);
    return rc;
#else
    (void)ptx; (void)cubin; return -1;
#endif
}
// 64-bit FNV-1a over all unit PTX — the key for the linked-cubin disk cache below.
static unsigned long long rvx_units_hash(const std::vector<std::string>& units) {
    unsigned long long h = 1469598103934665603ULL;
    for (const auto& u : units) for (unsigned char c : u) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static int rvx_load_module(const std::vector<std::string>& units, CUmodule* mod, CUfunction* fn) {
    const int verbose = getenv("RVX_STATS") ? 1 : 0;
    // Linked-cubin disk cache: the out-of-process ptxas path bypasses the driver's JIT cache, so without
    // this EVERY process start re-assembles all regions (~20-30 s for a full ~109k-word guest image). On a content
    // hash hit the linked cubin loads directly — zero ptxas spawns, zero linking.
    char cpath[300] = {0};
#ifdef _WIN32
    { char dir[MAX_PATH]; if (GetTempPathA(sizeof dir, dir))
        snprintf(cpath, sizeof cpath, "%srvcud_xblk_%016llx.cubin", dir,
                 rvx_units_hash(units) ^ (unsigned long long)rvx_ptxas_olvl()); }   // opt level keys the cache
    if (cpath[0]) if (FILE* cf = fopen(cpath, "rb")) {
        fseek(cf, 0, SEEK_END); long sz = ftell(cf); fseek(cf, 0, SEEK_SET);
        std::vector<char> blob((size_t)(sz > 0 ? sz : 0));
        bool ok = sz > 0 && fread(blob.data(), 1, (size_t)sz, cf) == (size_t)sz; fclose(cf);
        if (ok && cuModuleLoadData(mod, blob.data()) == CUDA_SUCCESS) {
            if (cuModuleGetFunction(fn, *mod, "xk") == CUDA_SUCCESS) return 0;
            cuModuleUnload(*mod);
        }                                                    // unreadable/stale → fall through and rebuild
    }
#endif
    char log[8192]; log[0]=0;
    std::vector<char> ilog(1<<16); ilog[0]=0;
    CUjit_option opt[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
                           CU_JIT_INFO_LOG_BUFFER, CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES, CU_JIT_LOG_VERBOSE };
    void* val[] = { log, (void*)(size_t)sizeof(log), ilog.data(), (void*)ilog.size(), (void*)(size_t)verbose };
    CUlinkState ls;
    CUresult r = cuLinkCreate(5, opt, val, &ls);
    if (r != CUDA_SUCCESS) { fprintf(stderr,"[xblk] cuLinkCreate: %d\n",(int)r); return -1; }
    std::vector<std::vector<char>> cubins(units.size());     // keep alive until cuLinkComplete
    // Assemble units in PARALLEL: each is an independent single-threaded ptxas -c child, so the
    // cold build scales with cores. Concurrency capped (default 4, RVX_PTXAS_PAR=1..16) — a big
    // brx-heavy region can transiently commit ~1 GB per child; the per-child watchdog still holds.
    std::vector<int> arc(units.size(), -1);
    {
        int par = 4;
        if (const char* p = getenv("RVX_PTXAS_PAR")) { int v = atoi(p); if (v >= 1 && v <= 16) par = v; }
        std::atomic<size_t> next{0};
        size_t nthr = units.size() < (size_t)par ? units.size() : (size_t)par;
        std::vector<std::thread> th;
        for (size_t t = 0; t < nthr; t++)
            th.emplace_back([&]{ for (size_t u; (u = next.fetch_add(1)) < units.size(); )
                                     arc[u] = rvx_assemble_unit(units[u], cubins[u]); });
        for (auto& x : th) x.join();
    }
    for (size_t u=0; u<units.size(); u++) {
        char nm[24]; snprintf(nm, sizeof nm, "xu%zu", u);
        if (arc[u] == 0)
            r = cuLinkAddData(ls, CU_JIT_INPUT_CUBIN, cubins[u].data(), cubins[u].size(), nm, 0, nullptr, nullptr);
        else if (units[u].size() < (size_t)256 * 1024)        // no standalone ptxas — in-driver fallback,
            r = cuLinkAddData(ls, CU_JIT_INPUT_PTX, (void*)units[u].c_str(), units[u].size()+1, nm, 0, nullptr, nullptr);
        else {                                                // …but NEVER for big units: the in-process
            // JIT has the same knee with no process isolation (13 GB commits observed historically).
            // Fail so the caller retries with smaller regions.
            fprintf(stderr,"[xblk] unit %zu (%zu KB) failed standalone assembly — rebuilding smaller\n", u, units[u].size()/1024);
            cuLinkDestroy(ls); return -2;
        }
        if (r != CUDA_SUCCESS) {
            fprintf(stderr,"[xblk] add (%s): %d %s\n", nm, (int)r, log); cuLinkDestroy(ls); return -2; }
    }
    void* cubin=nullptr; size_t csz=0;
    r = cuLinkComplete(ls, &cubin, &csz);
    if (r != CUDA_SUCCESS) {
        fprintf(stderr,"[xblk] link: %d %s\n", (int)r, log); cuLinkDestroy(ls); return -3; }
    if (verbose && ilog[0]) fprintf(stderr,"[xblk] link info log:\n%s\n", ilog.data());
    if (cpath[0]) if (FILE* cf = fopen(cpath, "wb")) { fwrite(cubin, 1, csz, cf); fclose(cf); }  // populate the disk cache
    r = cuModuleLoadData(mod, cubin);                        // cubin owned by ls — load before destroy
    cuLinkDestroy(ls);
    if (r != CUDA_SUCCESS) { fprintf(stderr,"[xblk] module load: %d\n",(int)r); return (int)r; }
    if (cuModuleGetFunction(fn, *mod, "xk") != CUDA_SUCCESS) { cuModuleUnload(*mod); return -4; }
    return 0;
}

// Build the exec_block from the live (statically-translated) image. Compiles up to RVX_MAXW
// reachable+compilable words (env RVX_MAXW overrides). Single-core only (linear memory ⇒ byte addr ==
// %M + a). The old 24000-word cap existed only for the single-function ptxas memory knee; with per-unit
// out-of-process assembly the limit is gone — default to full coverage (measured guest image: 108650 words, 19 regions,
// ~1.3 GB per ptxas child, ~1.4 GB in-process; guest bench 15.5 → 27 MIPS, coverage WAS the bottleneck).
#ifndef RVX_MAXW
#define RVX_MAXW (1<<20)
#endif
static void rvxblk_build() {
    g_xblk_ok = 0;
    // Default ON again: MULTIMOD's per-region out-of-process assembly removed the single-function ptxas
    // memory knee (the old monolith transiently committed ~13 GB on any PTX change; now ≤~1.4 GB peak
    // with each region assembled in a spawned ptxas). RVX_OFF=1 still disables exec entirely.
    // Multi-core: one thread per guest core (same hybrid), word-interleaved addressing baked into the
    // generated PTX; lockstep guests run the regions in SIMT just like the interpreter.
    if (g_ncores < 1 || g_img.empty()) return;
    int maxw = RVX_MAXW; if (const char* e=getenv("RVX_MAXW")) { int v=atoi(e); if (v>0) maxw=v; }
    int N = g_pc2words;
    std::vector<uint8_t> comp(N, 0), seen(N, 0);
    // Reachability walk over the RAW image, seeded from every statically-translated leader (confirmed
    // code, including jalr-only-reachable function entries). Walking the raw instructions FILLS the
    // fusion-interior holes the translator leaves as BADUOP — without this, exec_block falls through to
    // an uncompiled word every few instructions and never runs a whole loop in-register. comp[w]=1 for
    // reachable + rvx_compilable words (contiguous regions); capped at RVX_MAXW.
    std::vector<int> stack; stack.reserve(4096);
    for (int w=0; w<N; w++) if (g_pc2uop_h[w] != RC_BADUOP) { if(!seen[w]){ seen[w]=1; stack.push_back(w); } }
    int cnt = 0;
    while (!stack.empty()) {
        int w = stack.back(); stack.pop_back();
        uint32_t instr = g_img[w], op = instr & 0x7F;
        if (rvx_compilable(op) && !comp[w] && cnt < maxw) { comp[w]=1; cnt++; }
        auto push=[&](int t){ if(t>=0 && t<N && !seen[t]){ seen[t]=1; stack.push_back(t); } };
        if (op==0x63)       { push(w+1); push((int)(((g_base+(uint32_t)w*4)+rv_bimm(instr)-g_base)>>2)); }   // branch: fall-through + target
        else if (op==0x6F)  { push((int)(((g_base+(uint32_t)w*4)+rv_jimm(instr)-g_base)>>2)); }              // jal: target only
        else if (op==0x67 || op==0x73) { /* jalr (dynamic) / system: stop — no static successor */ }
        else                { push(w+1); }                                                                   // sequential
    }
    if (cnt == 0) return;

    // GLOBAL sp-alignment proof. The host sets an aligned initial sp, so if EVERY reachable writer
    // of x2 provably preserves 4-alignment (addi sp,sp,4k / lui / auipc / aligned andi mask), then
    // sp ≡ 0 (mod 4) at ALL times by induction and the codegen lattice may seed sp ALIGNED at every
    // reset — stack spills/reloads then emit bare ld/st instead of the 14-instruction runtime-checked
    // fallback. Safety valves elsewhere: set_reg(x2, unaligned) and a translate-on-miss discovering a
    // bad writer both disable exec outright (g_xblk_ok=0) — sound, and never expected to trigger.
    bool sptrust = true;
    for (int w=0; w<N && sptrust; w++)
        if (seen[w] && !(sptrust = rvx_sp_writer_ok(g_img[w])) && getenv("RVX_STATS"))
            fprintf(stderr,"[xblk] sp-proof breaker: 0x%08X: %08X\n", g_base+(uint32_t)w*4, g_img[w]);

    // SPARSE dispatch entries: only pcs where execution can actually (re)enter the exec_block —
    // branch/jal targets, return sites (word after jal/jalr — also catches function entries, which
    // follow the previous function's terminator), and resume sites after uncompilable words (ecall
    // returns). The old rule (every translated leader) made 92% of compiled words brx targets, which
    // bloats every region's branchtargets table and resets the cross-instruction codegen state in
    // rvx_emit every ~1.1 instructions. The interpreter cooperates: uops at entry pcs carry uw bit7
    // and the kernel self-stops there, so the sparse set can't strand whole chunks in the interpreter.
    // Scanning ALL image words (incl. data decoding as branches) only ADDS spurious entries — safe.
    std::vector<uint8_t> disp(N, 0);
    auto entry=[&](int t){ if (t>=0 && t<N && comp[t] && g_pc2uop_h[t]!=RC_BADUOP) disp[t]=1; };
    for (int w=0; w<N; w++) {
        uint32_t in=g_img[w], op=in&0x7F; uint32_t pc=g_base+(uint32_t)w*4;
        uint32_t t=0; bool ht=false;
        if (op==0x63)      { t=pc+rv_bimm(in); ht=true; }
        else if (op==0x6F) { t=pc+rv_jimm(in); ht=true; entry(w+1); }    // target + return site
        else if (op==0x67 || !rvx_compilable(op)) entry(w+1);            // return / post-ecall resume site
        if (ht && t>=g_base && ((t-g_base)&3)==0) entry((int)((t-g_base)>>2));
    }
    int ndisp = 0;
    for (int w=0; w<N; w++) ndisp += disp[w];

    cuInit(0);
    CUcontext ctx=nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    CUmodule mod; CUfunction fn;
    std::vector<std::string> ptx; std::vector<uint32_t> pc2idx;
    // Build with assemble-failure RETRY: big regions are the fastest (r10) but live near the ptxas
    // knee — if a unit trips the child watchdog, halve the region size and regenerate. Worst case
    // ends at the long-proven 6000-word regions; only then give up (interpreter-only).
    {
        int defw = RVX_REGW; if (const char* e=getenv("RVX_REGW")) { int v=atoi(e); if (v>0) defw=v; }
        // Persisted knee discovery: when an assembly attempt trips the ptxas watchdog, remember the
        // region size that finally worked (keyed on the image content) — otherwise EVERY process
        // start replays the ~10 s failed attempt, and the idle stall also de-boosts the GPU clocks
        // right before the workload (measured as a fake ~25% slowdown).
        char kpath[300] = {0};
        {   unsigned long long h = 1469598103934665603ULL;
            const uint8_t* p = (const uint8_t*)g_img.data();
            for (size_t i = 0; i < g_img.size()*4; i++) { h ^= p[i]; h *= 1099511628211ULL; }
            h ^= (unsigned long long)defw * 2654435761u;
#ifdef _WIN32
            char dir[MAX_PATH]; if (GetTempPathA(sizeof dir, dir))
                snprintf(kpath, sizeof kpath, "%srvcud_regw_%016llx.txt", dir, h);
#endif
        }
        int tryw = 0;
        if (kpath[0]) if (FILE* kf = fopen(kpath, "rb")) {   // known-good size from a previous discovery
            int v = 0; if (fscanf(kf, "%d", &v) == 1 && v >= 6000 && v < defw) tryw = v;
            fclose(kf);
        }
        bool degraded = tryw != 0;
        for (;;) {
            g_rvx_regw_force = tryw;
            ptx.clear(); pc2idx.clear();
            rvx_codegen(ptx, pc2idx, g_img.data(), N, g_base, comp, disp, g_ncores, sptrust);
            g_rvx_regw_force = 0;
            if (const char* dp = getenv("RVX_DUMP"))         // offline ptxas experiments: <prefix><u>.ptx per unit
                for (size_t u=0; u<ptx.size(); u++) {
                    char fnm[512]; snprintf(fnm, sizeof fnm, "%s%zu.ptx", dp, u);
                    if (FILE* f = fopen(fnm, "wb")) { fwrite(ptx[u].data(), 1, ptx[u].size(), f); fclose(f); }
                }
            if (rvx_load_module(ptx, &mod, &fn) == 0) break;
            int cur = tryw ? tryw : defw;
            if (cur <= 6000) { fprintf(stderr,"[xblk] build failed (%d words)\n", cnt); return; }
            tryw = cur / 2;
            degraded = true;
            fprintf(stderr,"[xblk] assembly failed at region size %d — retrying with %d\n", cur, tryw);
        }
        if (degraded && tryw && kpath[0])
            if (FILE* kf = fopen(kpath, "wb")) { fprintf(kf, "%d", tryw); fclose(kf); }
    }

    void* d_p2i=nullptr;
    if (cudaMalloc(&d_p2i, (size_t)N*4) != cudaSuccess) { cuModuleUnload(mod); return; }
    cudaMemcpy(d_p2i, pc2idx.data(), (size_t)N*4, cudaMemcpyHostToDevice);
    g_xmod=mod; g_xfn=fn; g_x_pc2idx=d_p2i;
    g_xtab.assign(N, 0);
    for (int w=0; w<N; w++) g_xtab[w] = disp[w];   // hybrid enters exec_block only at dispatch-entry words
    // Stamp uw bit7 on the uops at entry pcs (the interpreter's self-stop sites). Skip if-converted
    // body uops (RP_GP/GNP: their guest pc is not an architectural re-entry point mid-region) and
    // anything whose weight would collide with the flag (never expected; weights are tiny).
    { int nbits=0;
      for (int w=0; w<N; w++) if (disp[w]) {
          uint32_t ui = g_pc2uop_h[w]; if (ui & 0x80000000u) continue;
          uint32_t pr = (g_w0h[ui]>>26)&7;
          if (pr==RP_GP || pr==RP_GNP) continue;
          if (g_uwh[ui] & 0x80) continue;
          g_uwh[ui] |= 0x80; nbits++;
      }
      cudaMemcpy(g_uw, g_uwh.data(), (size_t)g_nuops, cudaMemcpyHostToDevice);
      (void)nbits; }
    g_xblk_ok = getenv("RVX_OFF") ? 0 : 1;   // RVX_OFF=1 builds but disables exec (isolation probe)
    size_t psz = 0; for (auto& u : ptx) psz += u.size();
    int xnreg = -1, xlmem = -1;              // spill guardrail: ptxas register count + local (spill) bytes
    cuFuncGetAttribute(&xnreg, CU_FUNC_ATTRIBUTE_NUM_REGS, fn);
    cuFuncGetAttribute(&xlmem, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, fn);
    g_x_sptrust = sptrust ? 1 : 0;
    fprintf(stderr,"[xblk] built: %d compiled words, %d dispatch entries, %zu units, ~%zu KB PTX (exec %s), %d regs, %d B local, sp-proof %s\n",
            cnt, ndisp, ptx.size(), psz/1024, g_xblk_ok?"ON":"OFF", xnreg, xlmem, sptrust?"OK":"none");
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof pmc;     // ptxas memory-knee watch: peak build commit
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        fprintf(stderr,"[xblk] build peak: %.2f GB commit, %.2f GB working set\n",
                pmc.PeakPagefileUsage/1073741824.0, pmc.PeakWorkingSetSize/1073741824.0);
#endif
}

// Run the exec_block from g_state[0].pc for up to `budget` guest instructions. Updates g_state[0]
// (regs+pc, managed mem) and returns instructions retired, or -1 on launch error.
static std::vector<unsigned long long> g_prof_hist; static uint32_t g_prof_base = 0;
static long long rvxblk_step_once(long long budget) {
    *g_ret = 0;
    unsigned bud = (budget > 0x7fffffff) ? 0x7fffffffu : (unsigned)budget;
    void* args[] = { &g_mem, &g_state, &bud, &g_x_pc2idx, &g_ret };
    int block = g_ncores < 64 ? g_ncores : 64;                                // one thread per guest core (2-warp blocks)
    int grid  = (g_ncores + block - 1) / block;
    CUresult r = cuLaunchKernel(g_xfn, grid,1,1, block,1,1, 296*block,0, args, nullptr);   // per thread: 168 B XS spill slot + 128 B shadow return stack
    if (r != CUDA_SUCCESS) { fprintf(stderr,"[xblk] launch %d\n",(int)r); return -1; }
    if (cudaDeviceSynchronize() != cudaSuccess) { fprintf(stderr,"[xblk] sync fault\n"); return -1; }
    return (long long)*g_ret;
}
static long long rvxblk_step(long long budget) {
    if (!g_xblk_ok || !g_ret) return -1;
    // RVX_PROF=K: statistical guest-pc PROFILER — split the budget into K slices and histogram the
    // pc after each slice (pinned state, free to read). Relative bucket weights identify where guest
    // time goes; the extra launches distort absolute MIPS, so profile runs are never benchmarks.
    static int prof = -1;
    if (prof < 0) { const char* e = getenv("RVX_PROF"); prof = e ? atoi(e) : 0;
        if (prof > 1) atexit([]{
            std::vector<std::pair<unsigned long long,size_t>> top;
            for (size_t b = 0; b < g_prof_hist.size(); b++)
                if (g_prof_hist[b]) top.push_back({g_prof_hist[b], b});
            std::sort(top.rbegin(), top.rend());
            unsigned long long tot = 0; for (auto& t : top) tot += t.first;
            fprintf(stderr, "[prof] %llu samples, top buckets (256 B):\n", tot);
            for (size_t i = 0; i < top.size() && i < 24; i++)
                fprintf(stderr, "[prof]   0x%08X  %5.1f%%  (%llu)\n",
                        g_prof_base + (uint32_t)(top[i].second << 8),
                        100.0 * top[i].first / (double)tot, top[i].first);
        }); }
    if (prof > 1) {
        if (g_prof_hist.empty()) { g_prof_hist.assign(((size_t)g_pc2words * 4 + 255) >> 8, 0); g_prof_base = g_base; }
        long long total = 0, slice = budget / prof; if (slice < 1000) slice = 1000;
        while (total < budget) {
            long long did = rvxblk_step_once(slice < budget - total ? slice : budget - total);
            if (did < 0) return -1;
            total += did;
            uint32_t pc = g_state[0].pc & ~HALT_BIT;
            if (pc >= g_prof_base) { size_t b = (size_t)(pc - g_prof_base) >> 8;
                                     if (b < g_prof_hist.size()) g_prof_hist[b]++; }
            if (g_state[0].pc & HALT_BIT) break;
            if (did == 0) break;                       // not enterable — let the interpreter run
        }
        return total;
    }
    return rvxblk_step_once(budget);
}

// ── exec_block foundation: driver-API PTX execution pipeline ─────────────────
// Proves we can load+run hand-emitted PTX (the basis for cross-compiling hot rv32i blocks to PTX and
// dispatching them via an RC_EXECBLOCK uop). NOT the disallowed whole-program CUDA-as-PTX monolith —
// this loads small, hand/cross-emitted PTX directly. Returns 1 on success.
API int cuda_rvexec_selftest() {
    cudaFree(0);                                   // ensure the runtime primary context exists & is current
    const char* ptx =
        ".version 7.8\n.target sm_86\n.address_size 64\n"
        ".visible .entry k(.param .u64 p){\n"
        "  .reg .b64 %rd0; .reg .b32 %r0;\n"
        "  ld.param.u64 %rd0, [p];\n"
        "  mov.b32 %r0, 51966;\n"                  // 0xCAFE
        "  st.global.u32 [%rd0], %r0;\n"
        "  ret;\n}\n";
    cuInit(0);
    CUcontext ctx = nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    CUmodule mod; CUfunction fn;
    if (cuModuleLoadData(&mod, ptx) != CUDA_SUCCESS) { fprintf(stderr,"[xblk] PTX load fail\n"); return 0; }
    if (cuModuleGetFunction(&fn, mod, "k") != CUDA_SUCCESS) { cuModuleUnload(mod); return 0; }
    unsigned* d=nullptr; cudaMalloc(&d,4); cudaMemset(d,0,4);
    void* args[]={&d};
    CUresult lr = cuLaunchKernel(fn, 1,1,1, 1,1,1, 296,0, args, nullptr);
    cudaDeviceSynchronize();
    unsigned h=0; cudaMemcpy(&h,d,4,cudaMemcpyDeviceToHost);
    cudaFree(d); cuModuleUnload(mod);
    return (lr==CUDA_SUCCESS && h==0xCAFEu) ? 1 : 0;
}

// Offline end-to-end validation of the exec_block machine on a self-contained synthetic guest —
// exercises reg load/save, addi/add, a BACKWARD loop branch (budget-gated XDISP path), byte-wise
// sw+lw, and the system hand-off — WITHOUT touching the live guest context. Returns 1 on PASS.
//   x3=10; x1=0; x2=0; loop: x1+=x2; x2++; if(x2<x3) loop; mem[0x40]=x1; x4=mem[0x40]; ebreak
//   expect x1==45, x2==10, x4==45, pc==ebreak.
API int cuda_rvexec_blocktest() {
    cudaFree(0);
    auto Iimm=[](int imm,int rs1,int f3,int rd,int op){ return (uint32_t)((imm&0xFFF)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op; };
    auto R   =[](int f7,int rs2,int rs1,int f3,int rd){ return (uint32_t)(f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; };
    auto S   =[](int imm,int rs2,int rs1,int f3){ return (uint32_t)(((imm>>5)&0x7F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; };
    auto B   =[](int off,int rs2,int rs1,int f3){ uint32_t i=(uint32_t)off;
                 return (((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63; };
    std::vector<uint32_t> img = {
        Iimm(10,0,0,3,0x13),    // 0x00 addi x3,x0,10
        Iimm(0,0,0,1,0x13),     // 0x04 addi x1,x0,0
        Iimm(0,0,0,2,0x13),     // 0x08 addi x2,x0,0
        R(0,2,1,0,1),           // 0x0C add  x1,x1,x2
        Iimm(1,2,0,2,0x13),     // 0x10 addi x2,x2,1
        B(-8,3,2,4),            // 0x14 blt  x2,x3,0x0C
        S(0x40,1,0,2),          // 0x18 sw   x1,0x40(x0)
        Iimm(0x40,0,2,4,0x03),  // 0x1C lw   x4,0x40(x0)
        0x00100073u,            // 0x20 ebreak
    };
    int N=(int)img.size(); uint32_t base=0x1000;
    std::vector<uint8_t> comp(N,0);
    for (int w=0; w<N; w++) if (rvx_compilable(img[w]&0x7F)) comp[w]=1;   // word 8 (ebreak) stays 0
    std::vector<std::string> ptx; std::vector<uint32_t> pc2idx;
    rvx_codegen(ptx, pc2idx, img.data(), N, base, comp, comp, 1, false);   // tests: every word a dispatch entry, no sp proof

    cuInit(0);
    CUcontext ctx=nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    CUmodule mod; CUfunction fn;
    if (rvx_load_module(ptx, &mod, &fn) != 0) { fprintf(stderr,"[xblk] blocktest: module build failed\n"); return 0; }

    CoreState st; memset(&st,0,sizeof st); st.pc=base;
    CoreState* d_st=nullptr; cudaMalloc(&d_st,sizeof st); cudaMemcpy(d_st,&st,sizeof st,cudaMemcpyHostToDevice);
    void* d_mem=nullptr; cudaMalloc(&d_mem,256); cudaMemset(d_mem,0,256);
    void* d_p2i=nullptr; cudaMalloc(&d_p2i,(size_t)N*4); cudaMemcpy(d_p2i,pc2idx.data(),(size_t)N*4,cudaMemcpyHostToDevice);
    unsigned* d_ret=nullptr; cudaMalloc(&d_ret,4); cudaMemset(d_ret,0,4);
    unsigned bud=1000;
    void* args[]={&d_mem,&d_st,&bud,&d_p2i,&d_ret};
    CUresult lr=cuLaunchKernel(fn,1,1,1,1,1,1,296,0,args,nullptr);
    cudaError_t se=cudaDeviceSynchronize();
    cudaMemcpy(&st,d_st,sizeof st,cudaMemcpyDeviceToHost);
    unsigned ret=0; cudaMemcpy(&ret,d_ret,4,cudaMemcpyDeviceToHost);
    unsigned m40=0; cudaMemcpy(&m40,(char*)d_mem+0x40,4,cudaMemcpyDeviceToHost);
    cudaFree(d_st); cudaFree(d_mem); cudaFree(d_p2i); cudaFree(d_ret); cuModuleUnload(mod);
    int ok = (lr==CUDA_SUCCESS && se==cudaSuccess &&
              st.regs[1]==45 && st.regs[2]==10 && st.regs[4]==45 && m40==45 && st.pc==base+0x20);
    fprintf(stderr,"[xblk] blocktest x1=%u x2=%u x4=%u mem[0x40]=%u pc=0x%X ret=%u lr=%d se=%d -> %s\n",
            st.regs[1],st.regs[2],st.regs[4],m40,st.pc,ret,(int)lr,(int)se, ok?"PASS":"FAIL");
    return ok;
}

// Reference RV32I single-step (host, independent of rvx_emit) — returns next pc (0x8.. = halt).
static uint32_t rvx_ref_step(uint32_t* R, uint8_t* M, uint32_t pc, uint32_t instr) {
    uint32_t op=instr&0x7F, rd=(instr>>7)&0x1F, f3=(instr>>12)&7, rs1=(instr>>15)&0x1F, rs2=(instr>>20)&0x1F, f7=(instr>>25)&0x7F;
    auto wr=[&](uint32_t v){ if(rd) R[rd]=v; };
    int32_t i_=(int32_t)rv_iimm(instr);
    switch(op){
      case 0x37: wr(instr&0xFFFFF000u); return pc+4;
      case 0x17: wr(pc+(instr&0xFFFFF000u)); return pc+4;
      case 0x6F: { uint32_t t=pc+rv_jimm(instr); wr(pc+4); return t; }
      case 0x67: { uint32_t t=(R[rs1]+(uint32_t)i_)&~1u; wr(pc+4); return t; }
      case 0x63: { uint32_t u1=R[rs1],u2=R[rs2]; int32_t s1=(int32_t)u1,s2=(int32_t)u2; bool tk;
                   switch(f3){case 0:tk=u1==u2;break;case 1:tk=u1!=u2;break;case 4:tk=s1<s2;break;case 5:tk=s1>=s2;break;case 6:tk=u1<u2;break;default:tk=u1>=u2;}
                   return tk ? pc+rv_bimm(instr) : pc+4; }
      case 0x03: { uint32_t a=R[rs1]+(uint32_t)i_, v;
                   switch(f3){case 0:v=(uint32_t)(int8_t)M[a];break;case 1:v=(uint32_t)(int16_t)(uint16_t)(M[a]|(M[a+1]<<8));break;
                              case 2:v=M[a]|(M[a+1]<<8)|(M[a+2]<<16)|(M[a+3]<<24);break;case 4:v=M[a];break;default:v=M[a]|(M[a+1]<<8);}
                   wr(v); return pc+4; }
      case 0x23: { uint32_t a=R[rs1]+rv_simm(instr), v=R[rs2];
                   switch(f3){case 0:M[a]=(uint8_t)v;break;case 1:M[a]=(uint8_t)v;M[a+1]=(uint8_t)(v>>8);break;
                              default:M[a]=(uint8_t)v;M[a+1]=(uint8_t)(v>>8);M[a+2]=(uint8_t)(v>>16);M[a+3]=(uint8_t)(v>>24);}
                   return pc+4; }
      case 0x13: { uint32_t u1=R[rs1],sh=(uint32_t)i_&0x1F,v;
                   switch(f3){case 0:v=u1+(uint32_t)i_;break;case 2:v=((int32_t)u1<i_)?1:0;break;case 3:v=(u1<(uint32_t)i_)?1:0;break;
                              case 4:v=u1^(uint32_t)i_;break;case 6:v=u1|(uint32_t)i_;break;case 7:v=u1&(uint32_t)i_;break;
                              case 1:v=u1<<sh;break;default:v=(f7==0x20)?(uint32_t)((int32_t)u1>>sh):(u1>>sh);}
                   wr(v); return pc+4; }
      case 0x33: { uint32_t u1=R[rs1],u2=R[rs2],sh=u2&31,v;
                   if (f7==0x01) {                          // M extension (exact RV32M semantics)
                     int32_t s1=(int32_t)u1,s2=(int32_t)u2;
                     switch(f3){
                       case 0: v=u1*u2; break;
                       case 1: v=(uint32_t)(((int64_t)s1*(int64_t)s2)>>32); break;
                       case 2: v=(uint32_t)(((int64_t)s1*(int64_t)(uint64_t)u2)>>32); break;
                       case 3: v=(uint32_t)(((uint64_t)u1*(uint64_t)u2)>>32); break;
                       case 4: v=(u2==0)?0xFFFFFFFFu:(s1==(int32_t)0x80000000&&s2==-1)?0x80000000u:(uint32_t)(s1/s2); break;
                       case 5: v=(u2==0)?0xFFFFFFFFu:(u1/u2); break;
                       case 6: v=(u2==0)?u1:(s1==(int32_t)0x80000000&&s2==-1)?0u:(uint32_t)(s1%s2); break;
                       default:v=(u2==0)?u1:(u1%u2); }
                     wr(v); return pc+4;
                   }
                   switch(f3){case 0:v=(f7==0x20)?(u1-u2):(u1+u2);break;case 1:v=u1<<sh;break;case 2:v=((int32_t)u1<(int32_t)u2)?1:0;break;
                              case 3:v=(u1<u2)?1:0;break;case 4:v=u1^u2;break;case 5:v=(f7==0x20)?(uint32_t)((int32_t)u1>>sh):(u1>>sh);break;
                              case 6:v=u1|u2;break;default:v=u1&u2;}
                   wr(v); return pc+4; }
      case 0x0F: return pc+4;
      default: return 0x80000000u;
    }
}

// Differential fuzz: random RV32I programs with FORWARD-ONLY control (guaranteed to terminate) run on
// rvx_ref_step AND the exec_block; compares the full register file + pc + retired count. Isolates
// ALU/mem/branch-condition/forward-bra/lui/auipc bugs. Returns the number of FAILING programs (0 = pass).
API int cuda_rvexec_fuzz(int seed, int nprog) {
    cudaFree(0); cuInit(0);
    CUcontext ctx=nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    uint32_t st=(uint32_t)seed; auto rnd=[&](){ st=st*1664525u+1013904223u; return st; };
    const uint32_t base=0x1000, MEMB=8192, DLO=0x200, DHI=0x400;
    int fails=0;
    for (int p=0; p<nprog; p++) {
        int PL = 8 + (int)(rnd()%56);                       // 8..63 instrs
        std::vector<uint32_t> img(PL+1);
        for (int w=0; w<PL; w++) {
            uint32_t k=rnd()%100, rd=rnd()&31, rs1=rnd()&31, rs2=rnd()&31, instr;
            if (k<20) instr=(((rnd()&0xFFFFF)<<12))|(rd<<7)|((rnd()&1)?0x37:0x17);                       // lui/auipc
            else if (k<28 && w+1<PL) {                                                                    // forward branch
                uint32_t f3=(uint32_t)"\x00\x01\x04\x05\x06\x07"[rnd()%6];
                int hop=1+(int)(rnd()%((PL-w)>6?6:(PL-w))); uint32_t t=base+(uint32_t)(w+hop)*4; int off=(int)(t-(base+(uint32_t)w*4));
                uint32_t i=(uint32_t)off; instr=(((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63;
            }
            else if (k<32 && w+1<PL) {                                                                    // forward jal
                int hop=1+(int)(rnd()%((PL-w)>6?6:(PL-w))); uint32_t t=base+(uint32_t)(w+hop)*4; uint32_t off=t-(base+(uint32_t)w*4);
                uint32_t i=off; instr=(((i>>20)&1)<<31)|(((i>>1)&0x3FF)<<21)|(((i>>11)&1)<<20)|(((i>>12)&0xFF)<<12)|(rd<<7)|0x6F;
            }
            else if (k<50) { uint32_t f3=rnd()%5, ty=(uint32_t)"\x00\x01\x02\x04\x05"[f3];                // load (base x0, imm in scratch)
                uint32_t imm=DLO+(rnd()%(DHI-DLO)); instr=(imm<<20)|(0<<15)|(ty<<12)|(rd<<7)|0x03; }
            else if (k<68) { uint32_t f3=rnd()%3; uint32_t imm=DLO+(rnd()%(DHI-DLO));                      // store
                instr=(((imm>>5)&0x7F)<<25)|(rs2<<20)|(0<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; }
            else if (k<84) { uint32_t f3=rnd()&7, imm12=rnd()&0xFFF, f7=(f3==5&&(rnd()&1))?0x20:0;        // op-imm
                instr=(imm12<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; if(f3==1||f3==5) instr=(f7<<25)|((rnd()&0x1F)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
            else { uint32_t f3=rnd()&7, f7;                                                                // op (R) — incl. M extension
                if (rnd()%10 < 3) f7=0x01;                                                                 // mul/mulh[s]u/div[u]/rem[u]
                else f7=((f3==0||f3==5)&&(rnd()&1))?0x20:0;
                instr=(f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; }
            img[w]=instr;
        }
        img[PL]=0x00100073u;                                                                              // ebreak terminator
        int N=PL+1;
        // reference run
        uint32_t R[32]={0}; std::vector<uint8_t> M(MEMB,0); uint32_t pc=base; long long rs=0;
        for (int g=0; g<200000; g++){ int w=(int)((pc-base)>>2); if(w<0||w>=N) break; uint32_t ni=rvx_ref_step(R,M.data(),pc,img[w]); if(ni&0x80000000u){break;} pc=ni; R[0]=0; rs++; }
        uint32_t ref_pc=pc;
        // exec_block run
        std::vector<uint8_t> comp(N,0); for(int w=0;w<PL;w++) comp[w]=1;
        std::vector<std::string> ptx; std::vector<uint32_t> pc2idx; rvx_codegen(ptx,pc2idx,img.data(),N,base,comp,comp,1,false);
        CUmodule mod; CUfunction fn; if(rvx_load_module(ptx,&mod,&fn)!=0){ fprintf(stderr,"[fuzz] prog %d module fail\n",p); fails++; continue; }
        CoreState cs; memset(&cs,0,sizeof cs); cs.pc=base;
        CoreState* d_st=nullptr; cudaMalloc(&d_st,sizeof cs); cudaMemcpy(d_st,&cs,sizeof cs,cudaMemcpyHostToDevice);
        void* d_mem=nullptr; cudaMalloc(&d_mem,MEMB); cudaMemset(d_mem,0,MEMB);
        void* d_p2i=nullptr; cudaMalloc(&d_p2i,(size_t)N*4); cudaMemcpy(d_p2i,pc2idx.data(),(size_t)N*4,cudaMemcpyHostToDevice);
        unsigned* d_ret=nullptr; cudaMalloc(&d_ret,4); cudaMemset(d_ret,0,4);
        unsigned bud=300000; void* args[]={&d_mem,&d_st,&bud,&d_p2i,&d_ret};
        CUresult lr=cuLaunchKernel(fn,1,1,1,1,1,1,296,0,args,nullptr); cudaError_t se=cudaDeviceSynchronize();
        cudaMemcpy(&cs,d_st,sizeof cs,cudaMemcpyDeviceToHost); unsigned ret=0; cudaMemcpy(&ret,d_ret,4,cudaMemcpyDeviceToHost);
        cudaFree(d_st); cudaFree(d_mem); cudaFree(d_p2i); cudaFree(d_ret); cuModuleUnload(mod);
        bool ok = (lr==CUDA_SUCCESS && se==cudaSuccess && cs.pc==ref_pc && (long long)ret==rs);
        if (ok) for(int r=1;r<32;r++) if(cs.regs[r]!=R[r]){ ok=false; break; }
        if (!ok) { fails++;
            fprintf(stderr,"[fuzz] FAIL prog %d (PL=%d) ref_pc=0x%X exec_pc=0x%X refsteps=%lld ret=%u lr=%d se=%d\n",p,PL,ref_pc,cs.pc,rs,ret,(int)lr,(int)se);
            for(int r=1;r<32;r++) if(cs.regs[r]!=R[r]) fprintf(stderr,"    x%d ref=%08X exec=%08X\n",r,R[r],cs.regs[r]);
            if (fails>=5) { fprintf(stderr,"[fuzz] stopping after 5 fails\n"); break; }
        }
    }
    fprintf(stderr,"[fuzz] %d programs, %d failures (seed=%d)\n", nprog, fails, seed);
    return fails;
}

// Differential fuzz #2: programs WITH backward branches/jal and JALR (target = x31+word*4, where x31 is
// set to `base` by a leading auipc — so jalr reaches any code word). Programs may loop; the reference runs
// with a step cap and only terminating runs are compared. exec_block budget = refsteps+slack so a control
// divergence surfaces as a pc/register mismatch instead of an unbounded loop. Returns # of FAILING programs.
API int cuda_rvexec_fuzz2(int seed, int nprog) {
    cudaFree(0); cuInit(0);
    CUcontext ctx=nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    uint32_t st=(uint32_t)seed; auto rnd=[&](){ st=st*1664525u+1013904223u; return st; };
    const uint32_t base=0x1000, MEMB=8192, DLO=0x200, DHI=0x400, STEPCAP=20000;
    int fails=0, compared=0;
    for (int p=0; p<nprog; p++) {
        int PL = 12 + (int)(rnd()%52);
        std::vector<uint32_t> img(PL+1);
        img[0] = (0u<<12)|(31<<7)|0x17;                     // auipc x31, 0  →  x31 = base
        for (int w=1; w<PL; w++) {
            uint32_t k=rnd()%100, rd=rnd()%31, rs1=rnd()&31, rs2=rnd()&31, instr;   // rd in 0..30 (never clobber x31)
            if (k<10) instr=(((rnd()&0xFFFFF)<<12))|(rd<<7)|((rnd()&1)?0x37:0x17);
            else if (k<24) {                                                         // branch (forward OR backward)
                uint32_t f3=(uint32_t)"\x00\x01\x04\x05\x06\x07"[rnd()%6];
                int tw=1+(int)(rnd()%(PL-1)); int off=(tw-w)*4; uint32_t i=(uint32_t)off;
                instr=(((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63;
            }
            else if (k<30) {                                                         // jalr rd, (tw*4)(x31)
                int tw=1+(int)(rnd()%(PL-1)); uint32_t imm=(uint32_t)(tw*4)&0xFFF;
                instr=(imm<<20)|(31<<15)|(0<<12)|(rd<<7)|0x67;
            }
            else if (k<36) {                                                         // jal (forward or backward)
                int tw=1+(int)(rnd()%(PL-1)); int off=(tw-w)*4; uint32_t i=(uint32_t)off;
                instr=(((i>>20)&1)<<31)|(((i>>1)&0x3FF)<<21)|(((i>>11)&1)<<20)|(((i>>12)&0xFF)<<12)|(rd<<7)|0x6F;
            }
            else if (k<52) { uint32_t f3=rnd()%5, ty=(uint32_t)"\x00\x01\x02\x04\x05"[f3]; uint32_t imm=DLO+(rnd()%(DHI-DLO)); instr=(imm<<20)|(0<<15)|(ty<<12)|(rd<<7)|0x03; }
            else if (k<66) { uint32_t f3=rnd()%3; uint32_t imm=DLO+(rnd()%(DHI-DLO)); instr=(((imm>>5)&0x7F)<<25)|(rs2<<20)|(0<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; }
            else if (k<84) { uint32_t f3=rnd()&7; if(f3==1||f3==5){ uint32_t f7=(f3==5&&(rnd()&1))?0x20:0; instr=(f7<<25)|((rnd()&0x1F)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; } else instr=((rnd()&0xFFF)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
            else { uint32_t f3=rnd()&7, f7=((f3==0||f3==5)&&(rnd()&1))?0x20:0; instr=(f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; }
            img[w]=instr;
        }
        img[PL]=0x00100073u; int N=PL+1;
        uint32_t R[32]={0}; std::vector<uint8_t> M(MEMB,0); uint32_t pc=base; long long rs=0; bool term=false;
        for (uint32_t g=0; g<STEPCAP; g++){ int w=(int)((pc-base)>>2); if(w<0||w>=N){term=true;break;} uint32_t ni=rvx_ref_step(R,M.data(),pc,img[w]); if(ni&0x80000000u){term=true;break;} pc=ni; R[0]=0; rs++; }
        if (!term) continue;                                  // looping program — can't validate, skip
        compared++;
        uint32_t ref_pc=pc;
        std::vector<uint8_t> comp(N,0); for(int w=0;w<PL;w++) comp[w]=1;
        std::vector<std::string> ptx; std::vector<uint32_t> pc2idx; rvx_codegen(ptx,pc2idx,img.data(),N,base,comp,comp,1,false);
        CUmodule mod; CUfunction fn; if(rvx_load_module(ptx,&mod,&fn)!=0){ fprintf(stderr,"[fuzz2] prog %d module fail\n",p); fails++; continue; }
        CoreState cs; memset(&cs,0,sizeof cs); cs.pc=base;
        CoreState* d_st=nullptr; cudaMalloc(&d_st,sizeof cs); cudaMemcpy(d_st,&cs,sizeof cs,cudaMemcpyHostToDevice);
        void* d_mem=nullptr; cudaMalloc(&d_mem,MEMB); cudaMemset(d_mem,0,MEMB);
        void* d_p2i=nullptr; cudaMalloc(&d_p2i,(size_t)N*4); cudaMemcpy(d_p2i,pc2idx.data(),(size_t)N*4,cudaMemcpyHostToDevice);
        unsigned* d_ret=nullptr; cudaMalloc(&d_ret,4); cudaMemset(d_ret,0,4);
        unsigned bud=(unsigned)rs+64; void* args[]={&d_mem,&d_st,&bud,&d_p2i,&d_ret};
        CUresult lr=cuLaunchKernel(fn,1,1,1,1,1,1,296,0,args,nullptr); cudaError_t se=cudaDeviceSynchronize();
        cudaMemcpy(&cs,d_st,sizeof cs,cudaMemcpyDeviceToHost); unsigned ret=0; cudaMemcpy(&ret,d_ret,4,cudaMemcpyDeviceToHost);
        cudaFree(d_st); cudaFree(d_mem); cudaFree(d_p2i); cudaFree(d_ret); cuModuleUnload(mod);
        bool ok = (lr==CUDA_SUCCESS && se==cudaSuccess && cs.pc==ref_pc && (long long)ret==rs);
        if (ok) for(int r=1;r<32;r++) if(cs.regs[r]!=R[r]){ ok=false; break; }
        if (!ok) { fails++;
            fprintf(stderr,"[fuzz2] FAIL prog %d (PL=%d) ref_pc=0x%X exec_pc=0x%X refsteps=%lld ret=%u lr=%d se=%d\n",p,PL,ref_pc,cs.pc,rs,ret,(int)lr,(int)se);
            for(int r=1;r<32;r++) if(cs.regs[r]!=R[r]) fprintf(stderr,"    x%d ref=%08X exec=%08X\n",r,R[r],cs.regs[r]);
            if (fails>=5) { fprintf(stderr,"[fuzz2] stopping after 5 fails\n"); break; }
        }
    }
    fprintf(stderr,"[fuzz2] %d programs, %d compared (terminating), %d failures (seed=%d)\n", nprog, compared, fails, seed);
    return fails;
}

// Differential fuzz #3: models the REAL hybrid (exec_block runs compiled words; an interpreter — here
// rvx_ref_step — owns everything uncompiled), with a RANDOM ~70% compiled subset so interior gaps,
// fall-through hand-offs, and re-entry via brx.idx are all exercised. Compares the hybrid's final state
// against a pure reference run. This is the exact failure shape large guest images hit. Returns # of FAILING programs.
API int cuda_rvexec_fuzz3(int seed, int nprog) {
    cudaFree(0); cuInit(0);
    CUcontext ctx=nullptr; cuCtxGetCurrent(&ctx);
    if (!ctx) { CUdevice dev; cuDeviceGet(&dev,0); cuDevicePrimaryCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx); }
    uint32_t st=(uint32_t)seed; auto rnd=[&](){ st=st*1664525u+1013904223u; return st; };
    const uint32_t base=0x1000, MEMB=8192, DLO=0x200, DHI=0x400;
    int fails=0;
    for (int p=0; p<nprog; p++) {
        int PL = 10 + (int)(rnd()%50);
        std::vector<uint32_t> img(PL+1);
        for (int w=0; w<PL; w++) {                          // FORWARD-only control ⇒ terminates
            uint32_t k=rnd()%100, rd=rnd()&31, rs1=rnd()&31, rs2=rnd()&31, instr;
            if (k<14) instr=(((rnd()&0xFFFFF)<<12))|(rd<<7)|((rnd()&1)?0x37:0x17);
            else if (k<26 && w+1<PL) { uint32_t f3=(uint32_t)"\x00\x01\x04\x05\x06\x07"[rnd()%6];
                int hop=1+(int)(rnd()%((PL-w)>6?6:(PL-w))); uint32_t i=(uint32_t)(hop*4);
                instr=(((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63; }
            else if (k<32 && w+1<PL) { int hop=1+(int)(rnd()%((PL-w)>6?6:(PL-w))); uint32_t i=(uint32_t)(hop*4);
                instr=(((i>>20)&1)<<31)|(((i>>1)&0x3FF)<<21)|(((i>>11)&1)<<20)|(((i>>12)&0xFF)<<12)|(rd<<7)|0x6F; }
            else if (k<50) { uint32_t f3=rnd()%5, ty=(uint32_t)"\x00\x01\x02\x04\x05"[f3]; uint32_t imm=DLO+(rnd()%(DHI-DLO)); instr=(imm<<20)|(0<<15)|(ty<<12)|(rd<<7)|0x03; }
            else if (k<66) { uint32_t f3=rnd()%3; uint32_t imm=DLO+(rnd()%(DHI-DLO)); instr=(((imm>>5)&0x7F)<<25)|(rs2<<20)|(0<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; }
            else if (k<84) { uint32_t f3=rnd()&7; if(f3==1||f3==5){ uint32_t f7=(f3==5&&(rnd()&1))?0x20:0; instr=(f7<<25)|((rnd()&0x1F)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; } else instr=((rnd()&0xFFF)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
            else { uint32_t f3=rnd()&7, f7=((f3==0||f3==5)&&(rnd()&1))?0x20:0; instr=(f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; }
            img[w]=instr;
        }
        img[PL]=0x00100073u; int N=PL+1;
        // pure reference
        uint32_t Rref[32]={0}; std::vector<uint8_t> Mref(MEMB,0); uint32_t pc=base;
        for (int g=0; g<200000; g++){ int w=(int)((pc-base)>>2); if(w<0||w>=N) break; uint32_t ni=rvx_ref_step(Rref,Mref.data(),pc,img[w]); if(ni&0x80000000u)break; pc=ni; Rref[0]=0; }
        uint32_t ref_pc=pc;
        // random compiled subset (≈70%); never compile the ebreak
        std::vector<uint8_t> comp(N,0); for(int w=0;w<PL;w++) comp[w]=(rnd()%100)<70;
        std::vector<std::string> ptx; std::vector<uint32_t> pc2idx; rvx_codegen(ptx,pc2idx,img.data(),N,base,comp,comp,1,false);
        CUmodule mod; CUfunction fn; if(rvx_load_module(ptx,&mod,&fn)!=0){ fprintf(stderr,"[fuzz3] prog %d module fail\n",p); fails++; continue; }
        void* d_mem=nullptr; cudaMalloc(&d_mem,MEMB); cudaMemset(d_mem,0,MEMB);
        void* d_p2i=nullptr; cudaMalloc(&d_p2i,(size_t)N*4); cudaMemcpy(d_p2i,pc2idx.data(),(size_t)N*4,cudaMemcpyHostToDevice);
        CoreState* d_st=nullptr; cudaMalloc(&d_st,sizeof(CoreState));
        unsigned* d_ret=nullptr; cudaMalloc(&d_ret,4);
        // hybrid: exec_block on compiled pc, rvx_ref_step on uncompiled pc (mirrors the real interpreter fallback)
        uint32_t Rh[32]={0}; std::vector<uint8_t> Mh(MEMB,0); pc=base; bool blew=false;
        for (int g=0; g<400000; g++) {
            int w=(int)((pc-base)>>2); if(w<0||w>=N) break;
            if (comp[w]) {
                CoreState cs; memset(&cs,0,sizeof cs); for(int r=0;r<32;r++) cs.regs[r]=Rh[r]; cs.pc=pc;
                cudaMemcpy(d_st,&cs,sizeof cs,cudaMemcpyHostToDevice);
                cudaMemcpy(d_mem,Mh.data(),MEMB,cudaMemcpyHostToDevice);
                cudaMemset(d_ret,0,4); unsigned bud=100000; void* args[]={&d_mem,&d_st,&bud,&d_p2i,&d_ret};
                CUresult lr=cuLaunchKernel(fn,1,1,1,1,1,1,296,0,args,nullptr); cudaError_t se=cudaDeviceSynchronize();
                if(lr!=CUDA_SUCCESS||se!=cudaSuccess){ blew=true; break; }
                cudaMemcpy(&cs,d_st,sizeof cs,cudaMemcpyDeviceToHost); cudaMemcpy(Mh.data(),d_mem,MEMB,cudaMemcpyDeviceToHost);
                for(int r=0;r<32;r++) Rh[r]=cs.regs[r]; Rh[0]=0; pc=cs.pc;
            } else {
                uint32_t ni=rvx_ref_step(Rh,Mh.data(),pc,img[w]); if(ni&0x80000000u) break; pc=ni; Rh[0]=0;
            }
        }
        cudaFree(d_mem); cudaFree(d_p2i); cudaFree(d_st); cudaFree(d_ret); cuModuleUnload(mod);
        bool ok = !blew && pc==ref_pc;
        if (ok) for(int r=1;r<32;r++) if(Rh[r]!=Rref[r]){ ok=false; break; }
        if (ok) { if (memcmp(Mh.data(),Mref.data(),MEMB)!=0) ok=false; }
        if (!ok) { fails++;
            fprintf(stderr,"[fuzz3] FAIL prog %d (PL=%d) ref_pc=0x%X hyb_pc=0x%X blew=%d\n",p,PL,ref_pc,pc,(int)blew);
            for(int r=1;r<32;r++) if(Rh[r]!=Rref[r]) fprintf(stderr,"    x%d ref=%08X hyb=%08X\n",r,Rref[r],Rh[r]);
            if (fails>=5) { fprintf(stderr,"[fuzz3] stopping after 5 fails\n"); break; }
        }
    }
    fprintf(stderr,"[fuzz3] %d programs, %d failures (seed=%d)\n", nprog, fails, seed);
    return fails;
}

// Differential fuzz #4: the FULL rvcud pipeline (translator incl. all fusion arms + the interpreter
// kernel ladder) vs rvx_ref_step, on forward-only random programs INCLUDING the M extension. The
// exec fuzzers (#1-#3) cover rvx_emit only — an interpreter-arm bug ships undetected if nothing
// dynamic exercises it (RC_MEXT was exactly that). Compares final regs + pc + the data scratch.
API int cuda_rvcud_fuzz(int seed, int nprog) {
    if (!g_state && cuda_rv32i_init(1, 1u << 20) != 0) return -1;
    _putenv("RVX_OFF=1");                          // interpreter-only: skip the per-program exec build
    uint32_t st = (uint32_t)seed; auto rnd = [&]() { st = st*1664525u + 1013904223u; return st; };
    const uint32_t base = 0x1000, DLO = 0x200, DHI = 0x400;
    int fails = 0;
    std::vector<uint8_t> zero(0x1000, 0);
    for (int p = 0; p < nprog; p++) {
        int PL = 8 + (int)(rnd() % 56);
        std::vector<uint32_t> img(PL + 1);
        for (int w = 0; w < PL; w++) {
            uint32_t k = rnd()%100, rd = rnd()&31, rs1 = rnd()&31, rs2 = rnd()&31, instr;
            if (k < 20) instr = (((rnd()&0xFFFFF)<<12)) | (rd<<7) | ((rnd()&1) ? 0x37 : 0x17);
            else if (k < 28 && w+1 < PL) {
                uint32_t f3 = (uint32_t)"\x00\x01\x04\x05\x06\x07"[rnd()%6];
                int hop = 1 + (int)(rnd() % ((PL-w) > 6 ? 6 : (PL-w))); uint32_t i = (uint32_t)(hop*4);
                instr = (((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63;
            }
            else if (k < 32 && w+1 < PL) {
                int hop = 1 + (int)(rnd() % ((PL-w) > 6 ? 6 : (PL-w))); uint32_t i = (uint32_t)(hop*4);
                instr = (((i>>20)&1)<<31)|(((i>>1)&0x3FF)<<21)|(((i>>11)&1)<<20)|(((i>>12)&0xFF)<<12)|(rd<<7)|0x6F;
            }
            else if (k < 50) { uint32_t f3 = rnd()%5, ty = (uint32_t)"\x00\x01\x02\x04\x05"[f3];
                uint32_t imm = DLO + (rnd() % (DHI-DLO)); instr = (imm<<20)|(0<<15)|(ty<<12)|(rd<<7)|0x03; }
            else if (k < 68) { uint32_t f3 = rnd()%3; uint32_t imm = DLO + (rnd() % (DHI-DLO));
                instr = (((imm>>5)&0x7F)<<25)|(rs2<<20)|(0<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; }
            else if (k < 84) { uint32_t f3 = rnd()&7;
                if (f3==1 || f3==5) { uint32_t f7 = (f3==5 && (rnd()&1)) ? 0x20 : 0;
                    instr = (f7<<25)|((rnd()&0x1F)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
                else instr = ((rnd()&0xFFF)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
            else { uint32_t f3 = rnd()&7, f7;
                if (rnd()%10 < 4) f7 = 0x01;                          // M: mul/mulh[s]u/div[u]/rem[u]
                else f7 = ((f3==0||f3==5) && (rnd()&1)) ? 0x20 : 0;
                instr = (f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; }
            img[w] = instr;
        }
        img[PL] = 0x00100073u;                                        // ebreak terminator
        int N = PL + 1;
        // reference run
        uint32_t R[32] = {0}; std::vector<uint8_t> M(8192, 0); uint32_t pc = base;
        for (int g = 0; g < 200000; g++) {
            int w = (int)((pc - base) >> 2); if (w < 0 || w >= N) break;
            uint32_t ni = rvx_ref_step(R, M.data(), pc, img[w]); if (ni & 0x80000000u) break;
            pc = ni; R[0] = 0;
        }
        uint32_t ref_pc = pc;
        // rvcud pipeline run (fusion arms at their defaults)
        if (cuda_rvcud_set_code(img.data(), (unsigned)N*4, base, base) != 0) { fails++; continue; }
        cuda_rv32i_write_mem(0, zero.data(), 0, 0x1000);
        for (int i = 0; i < 32; i++) cuda_rv32i_set_reg(0, i, 0);
        cuda_rv32i_set_entry(0, base);
        cuda_rvcud_step_all(400000);
        uint32_t epc = cuda_rv32i_get_pc(0) & 0x7FFFFFFFu;
        bool ok = (epc == ref_pc);
        for (int r2 = 1; r2 < 32; r2++) if (cuda_rv32i_get_reg(0, r2) != R[r2]) { ok = false; break; }
        if (ok) { std::vector<uint8_t> gm(DHI-DLO);
            cuda_rv32i_read_mem(0, gm.data(), DLO, DHI-DLO);
            if (memcmp(gm.data(), M.data()+DLO, DHI-DLO) != 0) ok = false; }
        if (!ok) { fails++;
            fprintf(stderr, "[fuzz4] FAIL prog %d (PL=%d) ref_pc=0x%X rvcud_pc=0x%X\n", p, PL, ref_pc, epc);
            for (int r2 = 1; r2 < 32; r2++) if (cuda_rv32i_get_reg(0, r2) != R[r2])
                fprintf(stderr, "    x%d ref=%08X rvcud=%08X\n", r2, R[r2], cuda_rv32i_get_reg(0, r2));
            if (fails >= 5) { fprintf(stderr, "[fuzz4] stopping after 5 fails\n"); break; }
        }
    }
    // Phase 2: programs WITH backward branches and JALR (fuzz2's generator) — exercises INCBR,
    // translate-on-miss, and the JALR ladder arm, which the forward-only phase never reaches.
    int compared = 0;
    for (int p = 0; p < nprog && fails < 5; p++) {
        int PL = 12 + (int)(rnd() % 52);
        std::vector<uint32_t> img(PL + 1);
        img[0] = (0u<<12) | (31<<7) | 0x17;                           // auipc x31,0 → x31 = base
        for (int w = 1; w < PL; w++) {
            uint32_t k = rnd()%100, rd = rnd()%31, rs1 = rnd()&31, rs2 = rnd()&31, instr;
            if (k < 10) instr = (((rnd()&0xFFFFF)<<12)) | (rd<<7) | ((rnd()&1) ? 0x37 : 0x17);
            else if (k < 24) { uint32_t f3 = (uint32_t)"\x00\x01\x04\x05\x06\x07"[rnd()%6];
                int tw = 1 + (int)(rnd() % (PL-1)); int off = (tw-w)*4; uint32_t i = (uint32_t)off;
                instr = (((i>>12)&1)<<31)|(((i>>5)&0x3F)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((i>>1)&0xF)<<8)|(((i>>11)&1)<<7)|0x63; }
            else if (k < 30) { int tw = 1 + (int)(rnd() % (PL-1)); uint32_t imm = (uint32_t)(tw*4)&0xFFF;
                instr = (imm<<20)|(31<<15)|(0<<12)|(rd<<7)|0x67; }
            else if (k < 36) { int tw = 1 + (int)(rnd() % (PL-1)); int off = (tw-w)*4; uint32_t i = (uint32_t)off;
                instr = (((i>>20)&1)<<31)|(((i>>1)&0x3FF)<<21)|(((i>>11)&1)<<20)|(((i>>12)&0xFF)<<12)|(rd<<7)|0x6F; }
            else if (k < 52) { uint32_t f3 = rnd()%5, ty = (uint32_t)"\x00\x01\x02\x04\x05"[f3];
                uint32_t imm = DLO + (rnd() % (DHI-DLO)); instr = (imm<<20)|(0<<15)|(ty<<12)|(rd<<7)|0x03; }
            else if (k < 66) { uint32_t f3 = rnd()%3; uint32_t imm = DLO + (rnd() % (DHI-DLO));
                instr = (((imm>>5)&0x7F)<<25)|(rs2<<20)|(0<<15)|(f3<<12)|((imm&0x1F)<<7)|0x23; }
            else if (k < 82) { uint32_t f3 = rnd()&7;
                if (f3==1 || f3==5) { uint32_t f7 = (f3==5 && (rnd()&1)) ? 0x20 : 0;
                    instr = (f7<<25)|((rnd()&0x1F)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
                else instr = ((rnd()&0xFFF)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x13; }
            else { uint32_t f3 = rnd()&7, f7;
                if (rnd()%10 < 4) f7 = 0x01;
                else f7 = ((f3==0||f3==5) && (rnd()&1)) ? 0x20 : 0;
                instr = (f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|0x33; }
            img[w] = instr;
        }
        img[PL] = 0x00100073u; int N = PL + 1;
        uint32_t R[32] = {0}; std::vector<uint8_t> M(8192, 0); uint32_t pc = base; bool term = false;
        for (uint32_t g = 0; g < 20000; g++) {
            int w = (int)((pc - base) >> 2); if (w < 0 || w >= N) { term = true; break; }
            uint32_t ni = rvx_ref_step(R, M.data(), pc, img[w]); if (ni & 0x80000000u) { term = true; break; }
            pc = ni; R[0] = 0;
        }
        if (!term) continue;                                          // looping program — skip
        compared++;
        uint32_t ref_pc = pc;
        if (cuda_rvcud_set_code(img.data(), (unsigned)N*4, base, base) != 0) { fails++; continue; }
        cuda_rv32i_write_mem(0, zero.data(), 0, 0x1000);
        for (int i = 0; i < 32; i++) cuda_rv32i_set_reg(0, i, 0);
        cuda_rv32i_set_entry(0, base);
        for (int c = 0; c < 64 && !(cuda_rv32i_get_pc(0) & 0x80000000u); c++)
            if (cuda_rvcud_step_all(40000) != 0) break;               // chunked: lets translate-on-miss fire
        uint32_t epc = cuda_rv32i_get_pc(0) & 0x7FFFFFFFu;
        bool ok = (epc == ref_pc);
        for (int r2 = 1; r2 < 32; r2++) if (cuda_rv32i_get_reg(0, r2) != R[r2]) { ok = false; break; }
        if (ok) { std::vector<uint8_t> gm(DHI-DLO);
            cuda_rv32i_read_mem(0, gm.data(), DLO, DHI-DLO);
            if (memcmp(gm.data(), M.data()+DLO, DHI-DLO) != 0) ok = false; }
        if (!ok) { fails++;
            fprintf(stderr, "[fuzz4b] FAIL prog %d (PL=%d) ref_pc=0x%X rvcud_pc=0x%X\n", p, PL, ref_pc, epc);
            for (int r2 = 1; r2 < 32; r2++) if (cuda_rv32i_get_reg(0, r2) != R[r2])
                fprintf(stderr, "    x%d ref=%08X rvcud=%08X\n", r2, R[r2], cuda_rv32i_get_reg(0, r2));
        }
    }
    _putenv("RVX_OFF=");
    fprintf(stderr, "[fuzz4] %d+%d programs (%d jalr-phase compared), %d failures (seed=%d)\n",
            nprog, nprog, compared, fails, seed);
    return fails;
}

API void cuda_rv32i_shutdown() {
#if RVCUD_HOTHIST
    if (g_hot_host && !g_u2pch.empty()) {
        cudaDeviceSynchronize();
        FILE* f = fopen("C:\\work\\RiscV\\RiscVEmulator\\RiscVEmulator-RV32I-CUDA\\Native\\hot.csv", "w");
        if (f) { fprintf(f, "ui,pc,class,count\n");
            for (int i = 0; i < g_nuops; i++)
                if (g_hot_host[i]) fprintf(f, "%d,%u,0x%02X,%llu\n", i, g_u2pch[i], g_w0h[i] & 0x7F, g_hot_host[i]);
            fclose(f); }
    }
#endif
    rvcud_free();
    if (g_ret) { cudaFreeHost((void*)g_ret); g_ret = nullptr; }
    if (g_iobox) { cudaFreeHost((void*)g_iobox); g_iobox = nullptr; }
    if (g_mem)  { cudaFree(g_mem);  g_mem  = nullptr; }
    if (g_state) { cudaFreeHost(g_state); g_state = nullptr; }
    g_ncores = 0;
}
