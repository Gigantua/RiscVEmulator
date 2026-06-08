#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cuda_runtime.h>

static constexpr uint32_t HALT_BIT = 0x80000000u;

struct CoreState { uint32_t regs[32], pc; };

// Per-core RAM is stored WORD-INTERLEAVED across cores: guest word w of core
// `id` lives at mem[(size_t)w*nc + id]. Consecutive lanes in a warp touching the
// same guest address hit consecutive words → one coalesced 128 B transaction.
// Sub-word access reads/masks the containing word(s); a core owns its own words,
// so the read-modify-write on a store has no cross-lane race.
template<class T> static __device__ __forceinline__ T ld_i(const uint32_t* m, int nc, int id, uint32_t a) {
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
template<class T> static __device__ __forceinline__ void st_i(uint32_t* m, int nc, int id, uint32_t a, T v) {
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

__global__ void __launch_bounds__(256)
rv32i_kernel(CoreState* st, uint32_t* mem, const uint2* dec, int ncores, int budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g  = st[id];
    extern __shared__ uint32_t s_regs[];
    uint32_t* const regs = &s_regs[threadIdx.x * 33];
    #pragma unroll
    for (int i = 0; i < 32; i++) regs[i] = g.regs[i];
    uint32_t pc = g.pc;

    for (int n = 0; n < budget && (int32_t)pc >= 0; n++) {
        // Two separate 32-bit loads (they pipeline better than one 64-bit load), but from the
        // interleaved {instr,imm} record so both hit the same cache line — half the L1 footprint.
        const uint32_t instr = __ldg(&dec[pc >> 2].x);   // raw instruction
        const uint32_t d0    = __ldg(&dec[pc >> 2].y);   // predecoded immediate
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
            if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) { pc |= HALT_BIT; continue; }
            r = alu(f3, u1, u2, (uint32_t)(s2 & 0x1F), f7 == 0x20, f7 == 0x20);
        }
        else if (op == 0x03) {
            uint32_t addr = (uint32_t)(s1 + (int32_t)d0);
            switch (f3) {
                case 0: r = (uint32_t)(int8_t) ld_i<uint8_t> (mem, ncores, id, addr); break;
                case 1: r = (uint32_t)(int16_t)ld_i<uint16_t>(mem, ncores, id, addr); break;
                case 2: r =                    ld_i<uint32_t>(mem, ncores, id, addr); break;
                case 4: r =                    ld_i<uint8_t> (mem, ncores, id, addr); break;
                case 5: r =                    ld_i<uint16_t>(mem, ncores, id, addr); break;
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
            switch (f3) { case 0: st_i<uint8_t>(mem,ncores,id,addr,(uint8_t)u2); break;
                          case 1: st_i<uint16_t>(mem,ncores,id,addr,(uint16_t)u2); break;
                          case 2: st_i<uint32_t>(mem,ncores,id,addr,u2); break;
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

// Predecode each code word into its pre-assembled, sign-extended immediate, so the
// hot loop never reconstructs B/J/S/I immediates (one broadcast load replaces the
// ~6-op branch/jump immediate assembly). Run once after the code image is set.
__global__ void predecode_kernel(const uint32_t* code, uint2* dec, int nwords) {
    int w = blockIdx.x * blockDim.x + threadIdx.x;
    if (w >= nwords) return;
    const uint32_t instr = code[w];
    const uint32_t op = instr & 0x7F;
    uint32_t d0;
    if (op == 0x63)                        // B-type
        d0 = (((instr>>31)&1u)<<12 | ((instr>>7)&1u)<<11 | ((instr>>25)&0x3Fu)<<5 | ((instr>>8)&0xFu)<<1)
           | ((instr & 0x80000000u) ? 0xFFFFE000u : 0u);
    else if (op == 0x6F)                   // J-type
        d0 = (((instr>>31)&1u)<<20 | ((instr>>12)&0xFFu)<<12 | ((instr>>20)&1u)<<11 | ((instr>>21)&0x3FFu)<<1)
           | ((instr & 0x80000000u) ? 0xFFE00000u : 0u);
    else if (op == 0x37 || op == 0x17)     // U-type
        d0 = instr & 0xFFFFF000u;
    else if (op == 0x23)                   // S-type
        d0 = (uint32_t)(((int32_t)(instr & 0xFE000000) >> 20) | (int32_t)((instr >> 7) & 0x1F));
    else                                   // I-type (and don't-care for R-type)
        d0 = (uint32_t)((int32_t)instr >> 20);
    dec[w] = make_uint2(instr, d0);
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
// Translator transform toggles (grind A/B switches). T1 switch-flatten inlines
// all switch arms as predicated uops → makes the divergent guest WORSE; default off.
#ifndef RVCUD_T1
#define RVCUD_T1 0
#endif
#ifndef RVCUD_T2
#define RVCUD_T2 1
#endif
#ifndef REGSTRIDE
#define REGSTRIDE 33          // shared-regfile stride (33 = bank-conflict-free for 32 banks)
#endif
// KEEP: counted-loop addi+branch → INCBR fusion. Measured +19% on the compute guest (159.6k vs
// 133.8k MIPS), neutral on data/diverge, bit-identical. Folded into the default build.
#ifndef RVCUD_NO_INCBR
#define IDEA_INCBR 1
#endif
#ifndef RVCUD_LB
#define RVCUD_LB 256          // __launch_bounds__ max threads/block hint
#endif
// ===================================================================
// Class codes are SPARSE (opcode-like), NOT a dense 0..N — a dense switch makes ptxas
// emit BRX (an indirect jump), which measured -35%. Sparse values keep the dispatch a
// frequency-ordered predicated compare-ladder, exactly like rv32i_kernel's op-ladder.
enum { RC_ALUI=0x13, RC_ALUR=0x33, RC_MULC=0x0B, RC_LOAD=0x03, RC_BR=0x63, RC_ST=0x23,
       RC_CONST=0x37, RC_LEA=0x1B, RC_ADDC=0x2B, RC_JAL=0x6F, RC_JALR=0x67, RC_NOP=0x0F,
       RC_SUB=0x3B, RC_MULR=0x53, RC_MULADD=0x43, RC_XSH=0x4B, RC_INCBR=0x5B, RC_ROT=0x6B,
       RC_XSHADD=0x77, RC_ANDSH=0x57, RC_ILL=0x00 };
       // RC_MULADD: r=root*C+reg (one IMAD). RC_XSH: r=rs^(rs<<|>>k). RC_INCBR: rc+=K; if(rc cmp rX) goto T. RC_ROT: r=rotl(rs,s) (slli+srli+or → 1 uop)
enum { RP_UNC=0, RP_GP=1, RP_GNP=2, RP_SETP=3, RP_SETSEL=4, RP_GSEL=5 };
static constexpr uint32_t RC_BADUOP = 0x80000000u;   // pc2uop sentinel: not a uop leader → halt on landing

// w0: class[6:0] rd[11:7] rs1[16:12] rs2[21:17] f3[24:22] sra[25] pred[28:26] selc[31:29]
#define RCW0(cls,rd,rs1,rs2,f3,sra,pred,selc) \
    ((uint32_t)(cls) | ((uint32_t)(rd)<<7) | ((uint32_t)(rs1)<<12) | ((uint32_t)(rs2)<<17) \
     | ((uint32_t)(f3)<<22) | ((uint32_t)(sra)<<25) \
     | ((uint32_t)(pred)<<26) | ((uint32_t)(selc)<<29))

__global__ void __launch_bounds__(RVCUD_LB)
rvcud_kernel(CoreState* __restrict__ st, uint32_t* __restrict__ mem, const uint2* __restrict__ uops, const uint8_t* __restrict__ uw,
             const uint32_t* __restrict__ pc2uop, const uint32_t* __restrict__ uop2pc,
             int ncores, int nwords, uint32_t base, int budget, unsigned long long* __restrict__ retd) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    extern __shared__ uint32_t s_regs[];
    uint32_t* const regs = &s_regs[threadIdx.x * REGSTRIDE];
    #pragma unroll
    for (int i = 0; i < 32; i++) regs[i] = g.regs[i];
    uint32_t pc = g.pc;
    // entry: map guest pc → uop index (halt if out of range or a non-leader landing)
    uint32_t ui;
    if ((int32_t)pc < 0)                  ui = HALT_BIT;
    else { uint32_t pw = (pc - base) >> 2;
           ui = (pw < (uint32_t)nwords) ? __ldg(&pc2uop[pw]) : HALT_BIT;
           if (ui & 0x80000000u) ui = HALT_BIT; }
    uint32_t P = 0, SEL = 0;             // predicate + selector (if-conversion / switch-flatten)

    int gi = 0;
#ifdef RVCUD_UNROLL
    #pragma unroll RVCUD_UNROLL
#endif
    for (; gi < budget && (int32_t)ui >= 0; ) {
#ifdef IDEA_FETCH64
        const uint2 _uw = __ldg(&uops[ui]); const uint32_t w0 = _uw.x, w1 = _uw.y;   // one 64-bit load
#elif defined(IDEA_LDG_CS)
        uint32_t w0, w1;                             // streaming-cache-hint loads (.cs)
        asm("ld.global.cs.u32 %0, [%1];" : "=r"(w0) : "l"(&uops[ui].x));
        asm("ld.global.cs.u32 %0, [%1];" : "=r"(w1) : "l"(&uops[ui].y));
#elif defined(IDEA_LDG_LU)
        uint32_t w0, w1;                             // last-use cache hint (.lu)
        asm("ld.global.lu.u32 %0, [%1];" : "=r"(w0) : "l"(&uops[ui].x));
        asm("ld.global.lu.u32 %0, [%1];" : "=r"(w1) : "l"(&uops[ui].y));
#else
        const uint32_t w0 = __ldg(&uops[ui].x);     // two 32-bit loads, one cache line (never LDG.E.64)
        const uint32_t w1 = __ldg(&uops[ui].y);
#endif
#ifdef IDEA_PREFETCH
        asm volatile("prefetch.global.L1 [%0];" :: "l"(uops + ui + 1));   // pull the next uop into L1 early
#endif
#ifdef IDEA_PREFETCH2
        asm volatile("prefetch.global.L1 [%0];" :: "l"(uops + ui + 2));   // prefetch two uops ahead
#endif
#ifdef IDEA_PREFETCH_L2
        asm volatile("prefetch.global.L2 [%0];" :: "l"(uops + ui + 1));   // prefetch next uop into L2
#endif
        const uint32_t cls  = w0 & 0x7F;
        const int      rd   = (w0 >> 7) & 0x1F;
        const uint32_t u1   = regs[(w0 >> 12) & 0x1F];
        const uint32_t u2   = regs[(w0 >> 17) & 0x1F];
        const uint32_t f3   = (w0 >> 22) & 7;
        const bool     sra  = (w0 >> 25) & 1;
        const uint32_t pred = (w0 >> 26) & 7;
        const uint32_t selc = (w0 >> 29) & 7;
        const int32_t  s1 = (int32_t)u1, s2 = (int32_t)u2;
        const int      wt = __ldg(&uw[ui]);          // guest-instructions this uop retires

        // predicate-producing uops: compute P / SEL, no rd write, fall through linearly
        if (pred == RP_SETP) {
            int t; switch (f3) { case 0:t=u1==u2;break; case 1:t=u1!=u2;break; case 4:t=s1<s2;break;
                                 case 5:t=s1>=s2;break; case 6:t=u1<u2;break; default:t=u1>=u2; }
            P = (selc & 1) ? (uint32_t)(!t) : (uint32_t)t;   // selc bit0 = invert (body-live polarity)
            ui += 1; gi += wt; continue;
        }
#if RVCUD_T1
        if (pred == RP_SETSEL) { SEL = (u1 >> (w1 & 31)) & ((w1 >> 8) & 0xFFu); ui += 1; gi += wt; continue; }  // w1 = shift | mask<<8
#endif
        uint32_t live = 1;
#if RVCUD_T1
        if (pred) live = (pred==RP_GP) ? P : (pred==RP_GNP) ? (P ^ 1u) : (uint32_t)(SEL == selc);
#else
        if (pred) live = (pred==RP_GP) ? P : (P ^ 1u);   // T1 off → pred ∈ {GP,GNP} here (GSEL/SETSEL never emitted)
#endif

        // Frequency-ordered dispatch ladder (hottest classes first → fewest predicted compares on
        // the common path). SUB/MULR are rare (never fire on these guests) → pushed to the tail.
        uint32_t r = 0, nui = ui + 1;
        if      (cls == RC_ALUI)  r = alu(f3, u1, w1, w1 & 0x1F, false, sra);
        else if (cls == RC_ALUR)  r = alu(f3, u1, u2, (uint32_t)(s2 & 0x1F), false, sra);
        else if (cls == RC_MULADD) r = u1 * w1 + u2;                             // affine ×const tree + live const-reg → 1 IMAD
        else if (cls == RC_XSH)   r = u1 ^ (sra ? (u1 >> (w1 & 31)) : (u1 << (w1 & 31)));  // xorshift step: rs ^ (rs<<|>>k)
#ifdef IDEA_ROT
        else if (cls == RC_ROT)   r = (u1 << (w1 & 31)) | (u1 >> ((32 - (w1 & 31)) & 31)); // rotate-left by w1
#endif
#ifdef IDEA_XSHADD
        else if (cls == RC_XSHADD) r = (u1 ^ (sra ? (u1 >> (w1 & 31)) : (u1 << (w1 & 31)))) + u2;  // xorshift step + add
#endif
#ifdef IDEA_ANDSH
        else if (cls == RC_ANDSH) { uint32_t tmp = sra ? (u1 >> (w1 & 31)) : (u1 << (w1 & 31));    // (rs<<|>>k) <logic> rt
                                    r = (f3==7) ? (tmp & u2) : (f3==6) ? (tmp | u2) : (tmp ^ u2); }
#endif
        else if (cls == RC_ADDC)  r = u1 + w1;
        else if (cls == RC_BR) {
            int t; switch (f3) { case 0:t=u1==u2;break; case 1:t=u1!=u2;break; case 4:t=s1<s2;break;
                                 case 5:t=s1>=s2;break; case 6:t=u1<u2;break; default:t=u1>=u2; }
            ui = t ? w1 : ui + 1; gi += wt; continue;                            // w1 = baked target uop-index
        }
#ifdef IDEA_INCBR
        else if (cls == RC_INCBR) {                                             // counted loop: rc += K; if (rc cmp rX) goto T
            int32_t inc = (int32_t)w1 >> 24;                                     // signed high byte = increment
            uint32_t tgt = w1 & 0x00FFFFFFu;                                     // low 24 = baked target uop-index (0xFFFFFF = unresolved → halt)
            uint32_t nv = u1 + (uint32_t)inc; int32_t sv = (int32_t)nv;
            if (rd) regs[rd] = nv;                                              // write the (incremented) counter
            int t; switch (f3) { case 0:t=nv==u2;break; case 1:t=nv!=u2;break; case 4:t=sv<s2;break;
                                 case 5:t=sv>=s2;break; case 6:t=nv<u2;break; default:t=nv>=u2; }
            ui = t ? (tgt==0x00FFFFFFu ? HALT_BIT : tgt) : ui + 1; gi += wt; continue;
        }
#endif
        else if (cls == RC_LOAD) { uint32_t a = (uint32_t)(s1 + (int32_t)w1);
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t>(mem,ncores,id,a); } }   // f3==5 (translator validated {0,1,2,4,5})
        else if (cls == RC_ST) { uint32_t a = (uint32_t)(s1 + (int32_t)w1);
            if (live) switch (f3) { case 0: st_i<uint8_t> (mem,ncores,id,a,(uint8_t)u2);  break;
                                    case 1: st_i<uint16_t>(mem,ncores,id,a,(uint16_t)u2); break;
                                    default:st_i<uint32_t>(mem,ncores,id,a,u2); }           // @p st.global when predicated
            ui += 1; gi += live ? wt : 0; continue; }    // count only if the guest would have run it
        else if (cls == RC_MULC)  r = u1 * w1;                                   // strength-reduced ×const → 1 IMAD
        else if (cls == RC_CONST) r = w1;
        else if (cls == RC_LEA)   r = (u1 << (w1 & 31)) + u2;                    // slli+add fused
        else if (cls == RC_SUB)   r = u1 - u2;                                   // R-type SUB (own class)
        else if (cls == RC_MULR)  r = u1 * u2;                                   // M-ext MUL (the stdlib's __mulsi3 imul)
        else if (cls == RC_JAL)  { if (rd && live) regs[rd] = __ldg(&uop2pc[ui]) + 4; ui = w1; gi += wt; continue; }  // skip the link load for `j` (rd==0)
        else if (cls == RC_JALR) { uint32_t tgt = (uint32_t)(s1 + (int32_t)w1) & ~1u;
            uint32_t tw = (tgt - base) >> 2;
            uint32_t n2 = (tw < (uint32_t)nwords) ? __ldg(&pc2uop[tw]) : RC_BADUOP;
            if (rd && live) regs[rd] = __ldg(&uop2pc[ui]) + 4;   // skip link load for indirect jump `jalr x0` (rd==0)
            ui = (n2 & 0x80000000u) ? HALT_BIT : n2; gi += wt; continue; }       // single broadcast pc2uop lookup, not brx.idx
        else if (cls == RC_NOP)  { ui += 1; gi += wt; continue; }
        else { ui |= HALT_BIT; continue; }                                       // RC_ILL

#ifdef IDEA_WB_SELP
        if (rd) regs[rd] = live ? r : regs[rd];      // selp form (reads regs[rd]) — for comparison
#else
        if (rd && live) regs[rd] = r;                // predicated store (@p st.shared) — no regs[rd] read-back
#endif
        ui = nui; gi += live ? wt : 0;               // a predicated-off body uop retires 0 guest instrs
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = regs[i];
    g.pc = ((int32_t)ui >= 0) ? __ldg(&uop2pc[ui]) : HALT_BIT;   // guest pc for read-back (guests don't halt here)
    if (id == 0 && retd) *retd = (unsigned long long)gi;          // retired guest-instructions (for the verify gate)
}

#define API extern "C" __declspec(dllexport)

static CoreState* g_state  = nullptr;
static uint32_t*  g_mem    = nullptr;   // word-interleaved RAM: [word*ncores + core]
static uint32_t*  g_code   = nullptr;   // staging for the raw code image
static uint2*     g_dec    = nullptr;   // predecoded {instr, imm} pairs — one 64-bit broadcast fetch/instr
static int        g_ncores = 0;
static int        g_words  = 0;         // code/dec words = memBytes/4

// rvcud device buffers (built by the host translator in cuda_rvcud_set_code)
static uint2*     g_uops    = nullptr;  // {w0,w1} per uop
static uint8_t*   g_uw      = nullptr;  // guest-instruction weight per uop
static uint32_t*  g_pc2uop  = nullptr;  // guest-word → uop index (or RC_BADUOP)
static uint32_t*  g_uop2pc  = nullptr;  // uop index → guest pc
static int        g_nuops   = 0;
static int        g_pc2words = 0;       // length of pc2uop[] = translated code words (NOT total memory words)
static uint32_t   g_base    = 0;
static unsigned long long* g_ret = nullptr;  // core-0 retired guest-instruction count (verify gate)

API int cuda_rv32i_init(int nCores, unsigned int memBytes) {
    g_ncores = nCores;
    cudaError_t e;
    if ((e = cudaMallocManaged(&g_state, (size_t)nCores * sizeof(CoreState))) != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_mem, (size_t)nCores * memBytes)) != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_code, memBytes)) != cudaSuccess) return (int)e;
    g_words = (int)(memBytes >> 2);
    if ((e = cudaMalloc(&g_dec, (size_t)g_words * sizeof(uint2))) != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    cudaMemset(g_mem, 0, (size_t)nCores * memBytes);
    cudaMemset(g_code, 0, memBytes);
    cudaMemset(g_dec, 0, (size_t)g_words * sizeof(uint2));
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}

API void cuda_rv32i_set_reg  (int core, int i, unsigned int v) { if (i) g_state[core].regs[i & 31] = v; }
API void cuda_rv32i_set_entry(int core, unsigned int pc)       { g_state[core].pc = pc; }
API void cuda_rv32i_set_halted(int core, int v) { if (v) g_state[core].pc |= HALT_BIT; else g_state[core].pc &= ~HALT_BIT; }

// Host I/O scatters/gathers words into the interleaved layout (one word per 2D
// "row", stride g_ncores words). off/len are word-aligned for all call sites.
API int cuda_rv32i_write_mem(int core, const void* src, unsigned int off, unsigned int len) {
    uint8_t* dst = (uint8_t*)(g_mem + (size_t)(off >> 2) * g_ncores + core);
    return (int)cudaMemcpy2D(dst, (size_t)g_ncores * 4, src, 4, 4, len >> 2, cudaMemcpyHostToDevice);
}
API int cuda_rv32i_read_mem(int core, void* dst, unsigned int off, unsigned int len) {
    const uint8_t* src = (const uint8_t*)(g_mem + (size_t)(off >> 2) * g_ncores + core);
    return (int)cudaMemcpy2D(dst, 4, src, (size_t)g_ncores * 4, 4, len >> 2, cudaMemcpyDeviceToHost);
}

// Shared read-only code image (contiguous, one copy for all cores). Valid when
// every core runs the same program — fetches broadcast from a single 8 KB region.
API int cuda_rv32i_set_code(const void* src, unsigned int len) {
    cudaError_t e = cudaMemcpy(g_code, src, len, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) return (int)e;
    int b = 256, g = (g_words + b - 1) / b;            // predecode the whole image once
    predecode_kernel<<<g, b>>>(g_code, g_dec, g_words);
    return (int)cudaDeviceSynchronize();
}

API int cuda_rv32i_step_all(int budget) {
    if (g_ncores <= 0) return 0;
    // Small (2-warp) blocks: at modest core counts this spreads work across many
    // more SMs than a 256-thread block (which would pile onto a handful of SMs and
    // leave the rest idle) — the interpreter is latency-bound, so SM coverage wins.
    int block = g_ncores < 64 ? g_ncores : 64;
    int grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 33 * sizeof(uint32_t);
    rv32i_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_dec, g_ncores, budget);
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
                   else if (f7==0x01 && f3==0) cls = RC_MULR;       // M-extension MUL (from runtime.c's __mulsi3)
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

#ifdef IDEA_INCBR
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
#endif

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
    int64_t co[32][NB]; int64_t im[32]; bool sdef[32];
    for (int i=0;i<32;i++){ sdef[i]=false; im[i]=0; for(int b=0;b<NB;b++) co[i][b]=0; }
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
    // Candidates: each time a fold produces a CLEAN multiply form (one root·C with C not a power of
    // two — pure shifts stay shifts — plus coef-1 units + imm) for a live-out reg, snapshot it. The
    // reg may be clobbered later (clang reuses regs), so we record the classification AT its def.
    struct Cand { int reg, pos, rootv, C, nu, unit[8]; int64_t K; };
    Cand cands[32]; int nc = 0;
    auto classify = [&](int reg)->void {                             // record co[reg] if a clean ×const
        if (nc >= 32) return;
        int rv=-1, un[8], n=0; uint32_t cc=0; bool clean=true;
        for (int b=1;b<NB;b++){ int64_t c=co[reg][b]; if(!c) continue;
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
        } else if (op==0x33 && f7==0x00 && f3==0 && rd) {            // add
            getsym(rs1,s1c,s1a); getsym(rs2,s2c,s2a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]+s2c[b]; im[rd]=s1a+s2a; folded=true;
        } else if (op==0x33 && f7==0x20 && f3==0 && rd) {            // sub
            getsym(rs1,s1c,s1a); getsym(rs2,s2c,s2a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]-s2c[b]; im[rd]=s1a-s2a; folded=true;
        } else if (op==0x13 && f3==0 && rd) {                        // addi (mv when K==0)
            getsym(rs1,s1c,s1a);
            for(int b=0;b<NB;b++) co[rd][b]=s1c[b]; im[rd]=s1a+(int64_t)(int32_t)rv_iimm(instr); folded=true;
        }
        if (folded) { sdef[rd]=true; if ((B.liveout[j]>>rd)&1u) { int prev=nc; classify(rd); if (nc>prev) cands[nc-1].pos=j; } }
        else if (rd) sdef[rd]=false;                                 // non-affine survivor → fresh basis
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

#ifdef IDEA_ADDADD
    // ── ADDADD — addi rd,rs,A ; addi rd,rd,B  (1st rd dead except for the 2nd) → addi rd,rs,A+B ──
    if (OPC(w)==0x13 && ((img[w]>>12)&7)==0 && w+1<N && !B.leader[w+1] && OPC(w+1)==0x13 && ((img[w+1]>>12)&7)==0) {
        uint32_t i1=img[w], i2=img[w+1];
        uint32_t rd1=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, rd2=(i2>>7)&0x1F, rs2=(i2>>15)&0x1F;
        if (rd1!=0 && rd1==rd2 && rs2==rd1 && rs!=rd1) {              // chained on rd1, rs untouched
            w0 = RCW0(RC_ADDC, rd1, rs, 0, 0, 0, RP_UNC, 0);
            w1 = rv_iimm(i1) + rv_iimm(i2); weight = 2; tgt = RC_NOTGT; return 2;
        }
    }
#endif
#ifdef IDEA_BRCMP
    // ── BRCMP — slt/sltu rd,a,b ; beq/bne rd,x0,T  (rd dead) → branch directly on a<b (no temp) ──
    if (OPC(w)==0x33 && w+1<N && !B.leader[w+1] && OPC(w+1)==0x63) {
        uint32_t i1=img[w], i2=img[w+1], sf3=(i1>>12)&7, sf7=(i1>>25)&0x7F;
        uint32_t rd=(i1>>7)&0x1F, a=(i1>>15)&0x1F, b=(i1>>20)&0x1F;
        uint32_t bf3=(i2>>12)&7, ba=(i2>>15)&0x1F, bb=(i2>>20)&0x1F;
        bool isslt = (sf7==0 && (sf3==2||sf3==3));                    // slt (2) / sltu (3)
        bool cmp0  = (bf3==0||bf3==1) && ((ba==rd&&bb==0)||(ba==0&&bb==rd));   // beq/bne rd,x0
        bool dead  = !(B.liveout[w+1] & (1u<<rd));
        if (isslt && cmp0 && rd!=0 && dead) {
            // bne(rd!=0) ⇒ a<b ; beq(rd==0) ⇒ a>=b. signed slt→blt/bge(4/5), unsigned→bltu/bgeu(6/7)
            uint32_t nf3 = (sf3==2) ? (bf3==1 ? 4u : 5u) : (bf3==1 ? 6u : 7u);
            uint32_t pcw = B.base + (uint32_t)(w+1)*4;
            w0 = RCW0(RC_BR, 0, a, b, nf3, 0, RP_UNC, 0);
            w1 = 0; weight = 2; tgt = (pcw + rv_bimm(i2) - B.base) >> 2; return 2;
        }
    }
#endif

#ifdef IDEA_ROT
    // ── ROT — rotate: slli ta,rs,s ; srli tb,rs,32-s ; or rd,{ta,tb}  (ta,tb dead) → rotl(rs,s) ──
    if ((OPC(w)==0x13) && w+2<N && !B.leader[w+1] && !B.leader[w+2] && OPC(w+1)==0x13 && OPC(w+2)==0x33) {
        uint32_t a=img[w], b=img[w+1], c=img[w+2];
        uint32_t af3=(a>>12)&7, bf3=(b>>12)&7, cf3=(c>>12)&7, cf7=(c>>25)&0x7F;
        uint32_t ta=(a>>7)&0x1F, ars=(a>>15)&0x1F, ash=(a>>20)&0x1F;
        uint32_t tb=(b>>7)&0x1F, brs=(b>>15)&0x1F, bsh=(b>>20)&0x1F;
        uint32_t rd=(c>>7)&0x1F, cra=(c>>15)&0x1F, crb=(c>>20)&0x1F;
        // accept (slli, srli) or (srli, slli); shifts of the same source summing to 32; OR of {ta,tb}
        bool shapes = ((af3==1 && bf3==5) || (af3==5 && bf3==1));
        if (shapes && cf3==6 && cf7==0 && ars==brs && (ash+bsh)==32 && ta!=0 && tb!=0 && ta!=tb &&
            ((cra==ta && crb==tb) || (cra==tb && crb==ta))) {
            uint32_t lo = B.liveout[w+2];
            bool dead = (!(lo&(1u<<ta)) || rd==ta) && (!(lo&(1u<<tb)) || rd==tb);
            uint32_t lsh = (af3==1) ? ash : bsh;            // the left-shift amount = rotate-left amount
            if (dead && ars!=ta && ars!=tb) {
                w0 = RCW0(RC_ROT, rd, ars, 0, 0, 0, RP_UNC, 0);
                w1 = lsh; weight = 3; tgt = RC_NOTGT;
                return 3;
            }
        }
    }
#endif

#ifdef IDEA_ANDSH
    // ── ANDSH — (slli|srli) t,rs,k ; (and|or|xor) rd,t,rv  (t dead) → rd = (rs<<|>>k) <logic> rv ──
    if (OPC(w)==0x13 && w+1<N && !B.leader[w+1] && OPC(w+1)==0x33) {
        uint32_t i1=img[w], i2=img[w+1], sf3=(i1>>12)&7, sf7=(i1>>25)&0x7F;
        bool left=(sf3==1), right=(sf3==5 && sf7==0);
        uint32_t t=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, sh=(i1>>20)&0x1F;
        uint32_t xf3=(i2>>12)&7, xf7=(i2>>25)&0x7F, rd=(i2>>7)&0x1F, ra=(i2>>15)&0x1F, rb=(i2>>20)&0x1F;
        bool logic = (xf3==4||xf3==6||xf3==7) && xf7==0;
        if ((left||right) && logic && t!=0 && (ra==t||rb==t)) {
            uint32_t rv = (ra==t) ? rb : ra;
            bool tdead = (rd==t) || !(B.liveout[w+1] & (1u<<t));
            if (rv!=t && rs!=t && tdead) {
                w0 = RCW0(RC_ANDSH, rd, rs, rv, xf3, (uint32_t)right, RP_UNC, 0);
                w1 = sh; weight = 2; tgt = RC_NOTGT; return 2;
            }
        }
    }
#endif
#ifdef IDEA_XSHADD
    // ── XSHADD — (slli|srli) t,rs,k ; xor x,{t,rs} ; add y,{x,rv}  → y = (rs^(rs<<|>>k)) + rv ──
    if (OPC(w)==0x13 && w+2<N && !B.leader[w+1] && !B.leader[w+2] && OPC(w+1)==0x33 && OPC(w+2)==0x33) {
        uint32_t i1=img[w], i2=img[w+1], i3=img[w+2];
        uint32_t sf3=(i1>>12)&7, sf7=(i1>>25)&0x7F; bool left=(sf3==1), right=(sf3==5&&sf7==0);
        uint32_t t=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, sh=(i1>>20)&0x1F;
        uint32_t xf3=(i2>>12)&7, xf7=(i2>>25)&0x7F, xrd=(i2>>7)&0x1F, xa=(i2>>15)&0x1F, xb=(i2>>20)&0x1F;
        uint32_t af3=(i3>>12)&7, af7=(i3>>25)&0x7F, ard=(i3>>7)&0x1F, aa=(i3>>15)&0x1F, ab=(i3>>20)&0x1F;
        bool xorok = (xf3==4 && xf7==0 && ((xa==t&&xb==rs)||(xa==rs&&xb==t)));
        bool addok = (af3==0 && af7==0 && (aa==xrd||ab==xrd));
        if ((left||right) && xorok && addok && t!=0 && xrd!=0) {
            uint32_t rv = (aa==xrd) ? ab : aa;
            bool tdead = !(B.liveout[w+2] & (1u<<t));            // t dead after the add
            bool xdead = (ard==xrd) || !(B.liveout[w+2] & (1u<<xrd));
            if (rs!=t && rv!=t && rv!=xrd && tdead && xdead) {
                w0 = RCW0(RC_XSHADD, ard, rs, rv, 0, (uint32_t)right, RP_UNC, 0);
                w1 = sh; weight = 3; tgt = RC_NOTGT; return 3;
            }
        }
    }
#endif

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

// Phase 4 (T1) — JUMP-TABLE SWITCH FLATTEN. Recognizes `jalr x0,rt,0` dispatched through a
// .rodata jump table (rt = lw[ TBASE + (idx<<2) ], selector idx = (x<<S1)>>S2), reads the N arm
// targets from the image, walks each PURE-ALU arm to the common merge, and replaces the indirect
// jump with: SETSEL (SEL=(x>>shift)&mask) + a predicated GUARD_SEL cascade of all arms + a jump to
// the merge. No brx.idx, divergence-free. Pushes uops; returns 1 (the jalr) consumed, or 0.
static int rvcud_try_switch(RvcudBuild& B, int w, uint32_t& ui) {
    const int N = B.nwords; const uint32_t* img = B.img;
    uint32_t instr = img[w];
    if ((instr & 0x7F) != 0x67 || ((instr>>7)&0x1F) != 0 || rv_iimm(instr) != 0) return 0;  // jalr x0,rt,0
    auto isW = [&](uint32_t op){ return op==0x13||op==0x33||op==0x03||op==0x37||op==0x17||op==0x6F||op==0x67; };
    auto findDef = [&](int from, uint32_t reg)->int {
        for (int j = from-1; j >= 0 && j >= from-32; j--) {
            uint32_t in = img[j], op = in&0x7F, rd = (in>>7)&0x1F;
            if (isW(op) && rd==reg && rd!=0) return j;
        } return -1;
    };
    uint32_t rt = (instr>>15)&0x1F;
    int jlw = findDef(w, rt); if (jlw<0 || (img[jlw]&0x7F)!=0x03 || ((img[jlw]>>12)&7)!=2) return 0; // lw rt,limm(rb)
    uint32_t rb = (img[jlw]>>15)&0x1F; int32_t limm = (int32_t)rv_iimm(img[jlw]);
    int jadd = findDef(jlw, rb);
    if (jadd<0 || (img[jadd]&0x7F)!=0x33 || ((img[jadd]>>12)&7)!=0 || ((img[jadd]>>25)&0x7F)!=0) return 0; // add rb,A,B
    uint32_t A=(img[jadd]>>15)&0x1F, Bb=(img[jadd]>>20)&0x1F;
    int jslli=-1; uint32_t tbReg=0;
    for (int t=0; t<2; t++) { uint32_t cand=t?Bb:A, other=t?A:Bb; int d=findDef(jadd,cand);
        if (d>=0 && (img[d]&0x7F)==0x13 && ((img[d]>>12)&7)==1 && ((img[d]>>20)&0x1F)==2) { jslli=d; tbReg=other; break; } }
    if (jslli<0) return 0;
    uint32_t idxReg=(img[jslli]>>15)&0x1F;                                 // scaled = idxReg<<2
    int jsr = findDef(jslli, idxReg);
    if (jsr<0 || (img[jsr]&0x7F)!=0x13 || ((img[jsr]>>12)&7)!=5 || ((img[jsr]>>25)&0x7F)!=0) return 0;  // srli idx,T,S2
    uint32_t S2=(img[jsr]>>20)&0x1F, Treg=(img[jsr]>>15)&0x1F;
    int jsl = findDef(jsr, Treg);
    if (jsl<0 || (img[jsl]&0x7F)!=0x13 || ((img[jsl]>>12)&7)!=1) return 0;  // slli T,X,S1
    uint32_t S1=(img[jsl]>>20)&0x1F, Xreg=(img[jsl]>>15)&0x1F;
    if (S2 < S1) return 0;
    uint32_t shift = S2 - S1, bits = 32 - S2; if (bits == 0 || bits > 3) return 0;   // ≤ 8-way
    uint32_t Nn = 1u << bits, mask = Nn - 1;
    // table base constant (loop-invariant; lui/auipc[+addi]) — search whole prefix
    auto constOf = [&](uint32_t reg, uint32_t& out)->bool {
        int d=-1; for (int j=jadd-1;j>=0;j--){ uint32_t in=img[j],op=in&0x7F,rd=(in>>7)&0x1F;
            if (isW(op)&&rd==reg&&rd!=0){d=j;break;} }
        if (d<0) return false; uint32_t in=img[d],op=in&0x7F,pcw=B.base+(uint32_t)d*4;
        if (op==0x37){ out=in&0xFFFFF000u; return true; }
        if (op==0x17){ out=pcw+(in&0xFFFFF000u); return true; }
        if (op==0x13 && ((in>>12)&7)==0){ uint32_t rs=(in>>15)&0x1F; int d2=-1;
            for (int j=d-1;j>=0;j--){ uint32_t i2=img[j],o2=i2&0x7F,r2=(i2>>7)&0x1F; if(isW(o2)&&r2==rs&&r2!=0){d2=j;break;} }
            if (d2<0) return false; uint32_t i2=img[d2],o2=i2&0x7F,p2=B.base+(uint32_t)d2*4; uint32_t b2;
            if (o2==0x37) b2=i2&0xFFFFF000u; else if (o2==0x17) b2=p2+(i2&0xFFFFF000u); else return false;
            out=b2+rv_iimm(in); return true; }
        return false;
    };
    uint32_t TB; if (!constOf(tbReg, TB)) return 0;
    uint32_t tbl = TB + (uint32_t)limm;                                    // table byte address
    int armStart[8];
    for (uint32_t k=0;k<Nn;k++){ uint32_t ea=tbl+k*4; if ((ea>>2)>=(uint32_t)N) return 0;
        uint32_t t=img[ea>>2], aw=(t-B.base)>>2; if ((int)aw>=N) return 0; armStart[k]=(int)aw; }
    // merge = target of arm0's first `j`
    int mergeWord=-1; { int cur=armStart[0], st=0;
        while (st++<16){ uint32_t in=img[cur],op=in&0x7F;
            if (op==0x6F && ((in>>7)&0x1F)==0){ mergeWord=(int)((B.base+(uint32_t)cur*4+rv_jimm(in)-B.base)>>2); break; }
            if (op==0x13||op==0x33||op==0x37||op==0x17){ cur++; continue; } break; } }
    if (mergeWord<0 || mergeWord>=N) return 0;
    // walk every arm: collect its ALU words + count guest instrs (ALU + j's), must reach mergeWord
    int collW[8][16], collN[8], armGC[8];
    for (uint32_t k=0;k<Nn;k++){ int cur=armStart[k], st=0, cn=0, gc=0;
        while (cur!=mergeWord && st++<16){
            if (cur<0||cur>=N){ cn=-1; break; }
            uint32_t in=img[cur],op=in&0x7F;
            if (op==0x6F && ((in>>7)&0x1F)==0){ gc++; cur=(int)((B.base+(uint32_t)cur*4+rv_jimm(in)-B.base)>>2); continue; }
            if (op==0x13||op==0x33||op==0x37||op==0x17){ if(cn>=16){cn=-1;break;} collW[k][cn++]=cur; gc++; cur++; continue; }
            cn=-1; break;   // load/store/branch/jalr/fence in arm → too complex, bail
        }
        if (cn<0 || cur!=mergeWord) return 0;
        collN[k]=cn; armGC[k]=gc;
    }
    // ── emit: SETSEL + GUARD_SEL cascade + merge JAL ──
    B.pc2uop[w]=ui;
    B.w0.push_back(RCW0(RC_NOP, 0, Xreg, 0, 0, 0, RP_SETSEL, 0));           // SEL=(x>>shift)&mask
    B.w1.push_back(shift | (mask<<8)); B.uw.push_back(1); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
    for (uint32_t k=0;k<Nn;k++){
        for (int i=0;i<collN[k];i++){ int cw=collW[k][i]; uint32_t a0,a1,t; uint8_t wt0; rvcud_classify(B,cw,a0,a1,wt0,t);
            a0 = (a0 & ~((uint32_t)7u<<26)) | ((uint32_t)RP_GSEL<<26);      // pred = GUARD_SEL
            a0 = (a0 & ~((uint32_t)7u<<29)) | ((uint32_t)k<<29);           // selc = case index
            uint8_t wt = (uint8_t)((i==collN[k]-1) ? (1 + (armGC[k]-collN[k])) : 1);  // last uop carries the arm's j-weight
            B.w0.push_back(a0); B.w1.push_back(a1); B.uw.push_back(wt);
            B.u2pc.push_back(B.base+(uint32_t)cw*4); B.tgt.push_back(t); ui++;
        }
    }
    B.w0.push_back(RCW0(RC_JAL, 0, 0, 0, 0, 0, RP_UNC, 0));                 // unconditional jump to merge (weight 0)
    B.w1.push_back(0); B.uw.push_back(0); B.u2pc.push_back(B.base+(uint32_t)w*4); B.tgt.push_back((uint32_t)mergeWord); ui++;
    return 1;
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
    B.pc2uop.assign(N, RC_BADUOP);
    uint32_t ui = 0;
    for (int w = 0; w < N; ) {
        int consumed = RVCUD_T1 ? rvcud_try_switch(B, w, ui) : 0;  // T1: jump-table switch → predicated cascade (default OFF — hurts divergent)
#ifdef IDEA_INCBR
        if (consumed == 0) consumed = rvcud_try_incbr(B, w, ui);   // counted-loop addi+branch → INCBR
#endif
        if (consumed == 0 && RVCUD_T2) consumed = rvcud_try_ifconv(B, w, ui);  // T2: if-conversion
        if (consumed == 0) consumed = rvcud_try_affine(B, w, ui);  // affine ×const fold → MULADD/MULC + ADD chain
        if (consumed == 0) {
            B.pc2uop[w] = ui;
            uint32_t a0, a1, t; uint8_t wt;
            consumed = rvcud_try_fuse(B, w, a0, a1, wt, t);   // ≥1; swallowed words stay BADUOP
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
#ifdef IDEA_INCBR
        else if (cls == RC_INCBR) {                                  // preserve K (high 8 bits); bake target into low 24
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x00FFFFFFu) : 0x00FFFFFFu;
            B.w1[i] = (B.w1[i] & 0xFF000000u) | idx;
        }
#endif
    }
}

static void rvcud_free() {
    if (g_uops)   { cudaFree(g_uops);   g_uops   = nullptr; }
    if (g_uw)     { cudaFree(g_uw);     g_uw     = nullptr; }
    if (g_pc2uop) { cudaFree(g_pc2uop); g_pc2uop = nullptr; }
    if (g_uop2pc) { cudaFree(g_uop2pc); g_uop2pc = nullptr; }
    g_nuops = 0;
}

// Translate the guest image into the rvcud uop stream and upload it. `base` is the
// guest image base (0x1000); `entry` the guest entry pc (so it stays a uop leader).
API int cuda_rvcud_set_code(const void* src, unsigned int len, unsigned int base, unsigned int entry) {
    rvcud_free();
    int N = (int)(len >> 2);
    RvcudBuild B; B.nwords = N; B.base = base; B.entry = entry;
    std::vector<uint32_t> words(N);
    memcpy(words.data(), src, (size_t)N * 4);
    B.img = words.data();
    rvcud_build(B);
    g_nuops = (int)B.w0.size(); g_base = base; g_pc2words = N;
    std::vector<uint2> uops(g_nuops);
    for (int i = 0; i < g_nuops; i++) uops[i] = make_uint2(B.w0[i], B.w1[i]);

    cudaError_t e;
    if ((e = cudaMalloc(&g_uops,   (size_t)g_nuops * sizeof(uint2)))   != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_uw,     (size_t)g_nuops))                   != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_uop2pc, (size_t)g_nuops * 4))              != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_pc2uop, (size_t)N * 4))                    != cudaSuccess) return (int)e;
    cudaMemcpy(g_uops,   uops.data(),     (size_t)g_nuops * sizeof(uint2), cudaMemcpyHostToDevice);
    cudaMemcpy(g_uw,     B.uw.data(),     (size_t)g_nuops,                cudaMemcpyHostToDevice);
    cudaMemcpy(g_uop2pc, B.u2pc.data(),   (size_t)g_nuops * 4,            cudaMemcpyHostToDevice);
    cudaMemcpy(g_pc2uop, B.pc2uop.data(), (size_t)N * 4,                  cudaMemcpyHostToDevice);
    return (int)cudaDeviceSynchronize();
}

// Budget is a GUEST-INSTRUCTION budget (the kernel accumulates per-uop weights), so the
// existing cores*budget*iters MIPS metric stays guest-MIPS and is apples-to-apples with rv32i.
API int cuda_rvcud_step_all(int budget) {
    if (g_ncores <= 0 || g_nuops <= 0) return -1;
    if (!g_ret) cudaMallocManaged(&g_ret, sizeof(unsigned long long));
#ifndef RVCUD_BLOCK
#define RVCUD_BLOCK 64
#endif
    int block = g_ncores < RVCUD_BLOCK ? g_ncores : RVCUD_BLOCK;
    int grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * REGSTRIDE * sizeof(uint32_t);
#ifdef IDEA_CARVEOUT_L1
    // The kernel needs only ~8KB shared (the regfile); bias the unified data cache toward L1 so the
    // broadcast uop/code-image fetch (the throughput bottleneck) gets the most L1.
    static bool s_carve=false; if(!s_carve){ s_carve=true;
        cudaFuncSetAttribute((const void*)rvcud_kernel, cudaFuncAttributePreferredSharedMemoryCarveout, cudaSharedmemCarveoutMaxL1); }
#endif
#ifdef IDEA_L2_PERSIST
    // Pin the read-only uop stream (read by every core) in a set-aside L2 region.
    static bool s_l2=false; if(!s_l2){ s_l2=true;
        cudaDeviceProp pr; cudaGetDeviceProperties(&pr,0);
        // Set aside only a small slice of L2 for the (few-KB) uop stream, so streaming-data guests
        // keep most of L2. L2_SETASIDE_KB=0 → max (steals L2; hurts divergent-memory guests).
#ifndef L2_SETASIDE_KB
#define L2_SETASIDE_KB 0
#endif
        size_t setaside = L2_SETASIDE_KB ? (size_t)L2_SETASIDE_KB*1024 : (size_t)pr.persistingL2CacheMaxSize;
        if(setaside>(size_t)pr.persistingL2CacheMaxSize) setaside=pr.persistingL2CacheMaxSize;
        if(setaside) cudaDeviceSetLimit(cudaLimitPersistingL2CacheSize, setaside);
        size_t bytes = (size_t)g_nuops*sizeof(uint2); if(bytes>(size_t)pr.accessPolicyMaxWindowSize) bytes=pr.accessPolicyMaxWindowSize;
        cudaStreamAttrValue av{}; av.accessPolicyWindow.base_ptr=g_uops; av.accessPolicyWindow.num_bytes=bytes;
        av.accessPolicyWindow.hitRatio=1.0f; av.accessPolicyWindow.hitProp=cudaAccessPropertyPersisting; av.accessPolicyWindow.missProp=cudaAccessPropertyStreaming;
        cudaStreamSetAttribute(0, cudaStreamAttributeAccessPolicyWindow, &av); }
#endif
    rvcud_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_uops, g_uw, g_pc2uop, g_uop2pc,
                                         g_ncores, g_pc2words, g_base, budget, g_ret);   // nwords = pc2uop length (code words)
    cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
    return le != cudaSuccess ? (int)le : (int)se;
}

// Guest-instructions retired by core 0 in the last cuda_rvcud_step_all (≈ budget + overshoot,
// since a fused uop retires several). The verify gate runs rv32i for exactly this many.
API unsigned long long cuda_rvcud_retired() { return g_ret ? *g_ret : 0ull; }

API void cuda_rv32i_shutdown() {
    rvcud_free();
    if (g_ret) { cudaFree(g_ret); g_ret = nullptr; }
    if (g_mem)  { cudaFree(g_mem);  g_mem  = nullptr; }
    if (g_code) { cudaFree(g_code); g_code = nullptr; }
    if (g_dec)  { cudaFree(g_dec);  g_dec  = nullptr; }
    if (g_state) { cudaFree(g_state); g_state = nullptr; }
    g_ncores = 0;
}
