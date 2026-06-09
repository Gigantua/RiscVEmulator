// ── EXPERIMENTAL FEATURE FLAGS (all default OFF → the committed-best path is byte-for-byte unchanged) ──
// Flip one on at the nvcc command line: -DRVCUD_CPASYNC=1  (each is independent; combine freely).
// Every flag preserves the rvcud↔rv32i bit-identical verify gate — they change cache policy, memory
// staging, or uop COUNT, never the architectural result. Validate each with the gate, then read the
// Doom "RV32I MIPS = uop-throughput × fusion-ratio" bench line (+FPS cross-check). See IDEA_BeyondTenMips.md.
//   RVCUD_CPASYNC   §1  cp.async/LDGSTS-stream a detected unit-stride memcpy loop's source through a
//                       shared double-buffer (overlaps the global read behind the store). Single-core
//                       only (ncores==1, where guest RAM is linear). GENERIC — keys on the copy-loop
//                       instruction pattern, not on any guest address.
//   RVCUD_BITFIELD  §8  fuse `srli rt,rs,k ; andi rd,rt,mask` → one BFE uop (rd = (rs>>k)&mask); the
//                       shift temp never reaches the shared regfile. GENERIC bit-field extract pattern.
#ifndef RVCUD_CPASYNC
#define RVCUD_CPASYNC 0
#endif
#ifndef RVCUD_BITFIELD
#define RVCUD_BITFIELD 0
#endif
#ifndef RVCUD_HOTHIST       // profiling only: count per-uop executions on core 0, dump CSV (ui,pc,class,count) on shutdown
#define RVCUD_HOTHIST 0
#endif
#ifndef RVCUD_LOADFWD       // #5  static redundant-load elimination: a load matching an earlier same-block
#define RVCUD_LOADFWD 0     //     load (base unchanged, no store between) → register copy, no memory op
#endif

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cuda_runtime.h>
#if RVCUD_CPASYNC
#include <cuda_pipeline.h>          // __pipeline_memcpy_async / commit / wait_prior (LDGSTS, sm_80+)
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

// Assemble the sign-extended, opcode-appropriate immediate for one instruction. Done inline in the
// fetch path so the interpreter reads live guest RAM (correct for self-modifying / JIT guests like
// TinyCC) rather than a stale predecoded copy.
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

__global__ void __launch_bounds__(256)
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
        const uint32_t instr = ld_i<uint32_t>(mem, ncores, id, pc);
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
       RC_COPYLOOP=0x6B, RC_MEXT=0x5D, RC_MULLOOP=0x5F, RC_BFE=0x09, RC_DIVLOOP=0x49,
       RC_MEMSET=0x39, RC_ILL=0x00 };
       // RC_MEMSET: byte-fill loop (libc memset/bzero: sb VAL,0(DST); DST++; bne DST,END) → 1 native loop uop.
       // RC_BFE (RVCUD_BITFIELD): rd = (rs >> k) & mask  — fused srli+andi; 0x09 is a free sparse code.
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
__global__ void __launch_bounds__(256)
rvcud_kernel(CoreState* __restrict__ st, uint32_t* __restrict__ mem, const uint2* __restrict__ uops, const uint8_t* __restrict__ uw,
             const uint32_t* __restrict__ pc2uop, const uint32_t* __restrict__ uop2pc,
             int ncores, int nwords, uint32_t base, int budget, unsigned long long* __restrict__ retd) {
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
        const int      wt = __ldg(&uw[ui]);          // guest-instructions this uop retires

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
        // Frequency-ordered for DOOM (the optimization target): hottest classes first so the common
        // path hits the fewest predicted compares. At ~48 cyc/uop each ladder compare is ~3%, so order
        // matters a lot. Universally-hot (ALUI/ALUR/LOAD/ST/BR) lead; Doom's fusions next; compute-only
        // (XSH/ADDC/MULC) and dormant-on-rv32i (MULR/MEXT) at the tail. Reorder is semantically identical.
        if      (cls == RC_ALUI)  r = alu(f3, u1, w1, w1 & 0x1F, false, sra);
        else if (cls == RC_ALUR)  r = alu(f3, u1, u2, (uint32_t)(s2 & 0x1F), false, sra);
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
        else if (cls == RC_BR) {
            int t; switch (f3) { case 0:t=u1==u2;break; case 1:t=u1!=u2;break; case 4:t=s1<s2;break;
                                 case 5:t=s1>=s2;break; case 6:t=u1<u2;break; default:t=u1>=u2; }
            ui = t ? w1 : ui + 1; gi += wt; continue;                            // w1 = baked target uop-index
        }
        else if (cls == RC_LOADPI) {                     // r = load[base]; base += w1  (load + post-increment)
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t> (mem,ncores,id,u1); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t>(mem,ncores,id,u1); break;
                          case 2: r=                   ld_i<uint32_t>(mem,ncores,id,u1); break;
                          case 4: r=                   ld_i<uint8_t> (mem,ncores,id,u1); break;
                          default:r=                   ld_i<uint16_t>(mem,ncores,id,u1); }
            regs[(w0>>12)&0x1F] = u1 + w1;               // rd != base (translator-enforced) → both writes safe
            pf_i(mem, ncores, id, u1 + w1); }            // prefetch next iteration's source (this loop's stride)
        else if (cls == RC_STOREPI) {                    // store[base] = rs2; base += w1  (store + post-increment)
            switch (f3) { case 0: st_i<uint8_t> (mem,ncores,id,u1,(uint8_t)u2);  break;
                          case 1: st_i<uint16_t>(mem,ncores,id,u1,(uint16_t)u2); break;
                          default:st_i<uint32_t>(mem,ncores,id,u1,u2); }
            regs[(w0>>12)&0x1F] = u1 + w1; ui += 1; gi += wt; continue; }
        else if (cls == RC_LDX) { uint32_t a = u1 + u2 + w1;          // rd = mem[ra+rb+imm]  (add + load → 1 uop)
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t>(mem,ncores,id,a); } }   // falls through to rd writeback
        else if (cls == RC_LDXS) {                                    // rd = mem[(ra<<k)+rb+imm]  (slli+add+load → 1 uop)
            uint32_t k = (w1>>24)&0x1F; int32_t imm = ((int32_t)(w1 & 0x00FFFFFF) << 8) >> 8;   // scale k; sign-ext 24→32
            uint32_t a = (u1 << k) + u2 + (uint32_t)imm;              // scaled address lives only in this CUDA register
            switch (f3) { case 0: r=(uint32_t)(int8_t) ld_i<uint8_t> (mem,ncores,id,a); break;
                          case 1: r=(uint32_t)(int16_t)ld_i<uint16_t>(mem,ncores,id,a); break;
                          case 2: r=                   ld_i<uint32_t>(mem,ncores,id,a); break;
                          case 4: r=                   ld_i<uint8_t> (mem,ncores,id,a); break;
                          default:r=                   ld_i<uint16_t>(mem,ncores,id,a); } }   // falls through to rd writeback
        else if (cls == RC_STXS) {                                    // mem[(ra<<k)+rb+imm] = rc  (slli+add+store → 1 uop)
            uint32_t k = (w1>>24)&0x1F; int32_t imm = ((int32_t)(w1 & 0x00FFFFFF) << 8) >> 8;
            uint32_t a = (u1 << k) + u2 + (uint32_t)imm; uint32_t val = regs[(w0>>7)&0x1F];   // rc carried in rd field
            switch (f3) { case 0: st_i<uint8_t> (mem,ncores,id,a,(uint8_t)val);  break;
                          case 1: st_i<uint16_t>(mem,ncores,id,a,(uint16_t)val); break;
                          default:st_i<uint32_t>(mem,ncores,id,a,val); }
            ui += 1; gi += wt; continue; }
        else if (cls == RC_MULADD) r = u1 * w1 + u2;                             // affine ×const tree + live const-reg → 1 IMAD
        else if (cls == RC_COPYPI) {                     // mem[dst]=mem[src]; rt=loaded; src+=Ks; dst+=Kd  (lb;sb;addi;addi → 1 uop)
            uint32_t t;                                  // u1=src base, u2=dst base; w1 = Ks(lo16) | Kd(hi16); rt = rd field
            switch (f3) {                                // t = sign/zero-extended load (mirrors lb/lbu/lh/lhu/lw); store uses low bits
                case 0: t=(uint32_t)(int8_t) ld_i<uint8_t> (mem,ncores,id,u1); st_i<uint8_t> (mem,ncores,id,u2,(uint8_t)t);  break;
                case 1: t=(uint32_t)(int16_t)ld_i<uint16_t>(mem,ncores,id,u1); st_i<uint16_t>(mem,ncores,id,u2,(uint16_t)t); break;
                case 2: t=                   ld_i<uint32_t>(mem,ncores,id,u1); st_i<uint32_t>(mem,ncores,id,u2,t);           break;
                case 4: t=                   ld_i<uint8_t> (mem,ncores,id,u1); st_i<uint8_t> (mem,ncores,id,u2,(uint8_t)t);  break;
                default:t=                   ld_i<uint16_t>(mem,ncores,id,u1); st_i<uint16_t>(mem,ncores,id,u2,(uint16_t)t); }
            if (rd) regs[rd] = t;                                                  // rt = loaded value (rt != src,dst)
            regs[(w0>>12)&0x1F] = u1 + (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFF);   // src += Ks
            regs[(w0>>17)&0x1F] = u2 + (uint32_t)(int32_t)(int16_t)(w1 >> 16);      // dst += Kd
            pf_i(mem, ncores, id, u1 + (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFF));  // prefetch next iteration's source
            ui += 1; gi += wt; continue; }
        else if (cls == RC_MULLOOP) {                    // whole shift-add software-multiply loop → 1 hardware multiply
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
            while (D != END && gi < budget) { st_i<uint8_t>(mem,ncores,id,D,(uint8_t)VAL); D += 1; gi += 4; }  // 4 instrs/iter
            regs[rd] = D; regs[nDr] = D;                 // DST→END; nD (temp) = END
            if (D == END) { uint32_t ft = w1 & 0x00FFFFFFu;
                if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 16; break; }   // 4 words; unresolved → translate-on-miss
                ui = ft; }
            continue; }
        else if (cls == RC_STX) { uint32_t a = u1 + u2 + w1;          // mem[ra+rb+imm] = rc  (add + store → 1 uop)
            uint32_t val = regs[(w0>>7)&0x1F];                        // rc carried in the rd field
            switch (f3) { case 0: st_i<uint8_t> (mem,ncores,id,a,(uint8_t)val);  break;
                          case 1: st_i<uint16_t>(mem,ncores,id,a,(uint16_t)val); break;
                          default:st_i<uint32_t>(mem,ncores,id,a,val); }
            ui += 1; gi += wt; continue; }
        else if (cls == RC_INCBR) {                                             // counted loop: rc += K; if (rc cmp rX) goto T
            int32_t inc = (int32_t)w1 >> 24;                                     // signed high byte = increment
            uint32_t tgt = w1 & 0x00FFFFFFu;                                     // low 24 = baked target uop-index (0xFFFFFF = unresolved → halt)
            uint32_t nv = u1 + (uint32_t)inc; int32_t sv = (int32_t)nv;
            if (rd) regs[rd] = nv;                                              // write the (incremented) counter
            int t; switch (f3) { case 0:t=nv==u2;break; case 1:t=nv!=u2;break; case 4:t=sv<s2;break;
                                 case 5:t=sv>=s2;break; case 6:t=nv<u2;break; default:t=nv>=u2; }
            ui = t ? (tgt==0x00FFFFFFu ? HALT_BIT : tgt) : ui + 1; gi += wt; continue;
        }
        else if (cls == RC_COPYLOOP) {                   // whole forward unit-stride byte memcpy loop, word-widened
            uint32_t s = u1, d = u2, L = regs[(w1>>24)&0x1F];  // src, dst, limit; counter = src or dst
            const bool cdst = (w0>>25)&1;                      // which pointer the bne compares to L
#if RVCUD_CPASYNC
            // cp.async/LDGSTS fast path (single-Doom only: ncores==1 ⇒ guest byte a lives at ((char*)mem)[a],
            // so the source region is contiguous and stage-able). Stream 16-B chunks through a shared
            // double-buffer — the global read of chunk N+1 overlaps the store of chunk N. Byte-for-byte
            // identical to the scalar copy below (same bytes, gi=5/byte), so the verify gate is unchanged.
            if (ncores == 1) {
                __shared__ uint32_t s_cp[2][4];            // two 16-B stages, one thread (block==1) → +32 B shared
                char* const gm = (char*)mem;
                while ((s & 15u) && (cdst ? d : s) != L && gi < budget) {   // align src to 16 B (cp.async.cg wants 16-B align)
                    st_i<uint8_t>(mem,ncores,id,d,(uint8_t)ld_i<uint8_t>(mem,ncores,id,s)); s++; d++; gi += 5; }
                int cur = 0; bool inflight = false;
                while ((L - (cdst ? d : s)) >= 16u && (((d>s)?(d-s):(s-d)) >= 16u) && gi < budget) {
                    if (!inflight) { __pipeline_memcpy_async(&s_cp[cur][0], gm + s, 16); __pipeline_commit(); }
                    int nxt = cur ^ 1;
                    bool haveNext = ((L - (cdst ? d : s) - 16u) >= 16u) && (gi + 80 < budget);
                    if (haveNext) { __pipeline_memcpy_async(&s_cp[nxt][0], gm + s + 16u, 16); __pipeline_commit(); }
                    __pipeline_wait_prior(haveNext ? 1 : 0);   // current stage filled
                    st_i<uint32_t>(mem,ncores,id,d,    s_cp[cur][0]);
                    st_i<uint32_t>(mem,ncores,id,d+4,  s_cp[cur][1]);
                    st_i<uint32_t>(mem,ncores,id,d+8,  s_cp[cur][2]);
                    st_i<uint32_t>(mem,ncores,id,d+12, s_cp[cur][3]);
                    s += 16u; d += 16u; gi += 80;              // 16 bytes × 5 guest instrs
                    cur = nxt; inflight = haveNext;
                }
                if (inflight) __pipeline_wait_prior(0);        // drain the unconsumed prefetch (tail re-reads from global)
            }
#endif
            while ((cdst ? d : s) != L && gi < budget) {       // scalar copy: base path AND the cp.async tail/overlap
                uint32_t cnt = cdst ? d : s, rem = L - cnt, adiff = (d > s) ? (d - s) : (s - d);
                pf_i(mem, ncores, id, s + 96);                 // prefetch source ~96B ahead of the copy cursor
                if (rem >= 4u && adiff >= 4u) {                // word copy: ≥4 to go AND non-overlapping within the word
                    st_i<uint32_t>(mem,ncores,id,d, ld_i<uint32_t>(mem,ncores,id,s));
                    s += 4; d += 4; gi += 20;                  // 4 byte-iterations (×5 guest instrs)
                } else {                                       // byte copy (tail / overlap)
                    st_i<uint8_t>(mem,ncores,id,d,(uint8_t)ld_i<uint8_t>(mem,ncores,id,s));
                    s += 1; d += 1; gi += 5;
                }
            }
            uint32_t lastb = ld_i<uint8_t>(mem,ncores,id,s-1);
            if (rd) regs[rd] = (f3==0) ? (uint32_t)(int8_t)lastb : lastb;          // rt = last loaded byte (may be live)
            regs[(w0>>12)&0x1F] = s; regs[(w0>>17)&0x1F] = d;                       // src, dst
            if ((cdst ? d : s) == L) {                                             // loop finished → fall through
                uint32_t ft = w1 & 0x00FFFFFFu;
                if (ft == 0x00FFFFFFu) { resume_pc = __ldg(&uop2pc[ui]) + 20; break; }   // unresolved → translate-on-miss
                ui = ft;
            }                                                                      // else budget-cut → ui stays, resume here
            continue; }
        else if (cls == RC_XSH)   r = u1 ^ (sra ? (u1 >> (w1 & 31)) : (u1 << (w1 & 31)));  // xorshift step: rs ^ (rs<<|>>k)
#if RVCUD_BITFIELD
        else if (cls == RC_BFE)   r = (u1 >> (w1 & 0x1F)) & (w1 >> 5);            // (rs>>k)&mask: fused srli+andi
#endif
        else if (cls == RC_ADDC)  r = u1 + w1;
        else if (cls == RC_MULC)  r = u1 * w1;                                   // strength-reduced ×const → 1 IMAD
        else if (cls == RC_LEA)   r = (u1 << (w1 & 31)) + u2;                    // slli+add fused
        else if (cls == RC_CONST) r = w1;
        else if (cls == RC_SUB)   r = u1 - u2;                                   // R-type SUB (own class)
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
        else if (cls == RC_NOP)  { ui += 1; gi += wt; continue; }
        else { ui |= HALT_BIT; continue; }                                       // RC_ILL

        if (rd && live) regs[rd] = r;                // predicated store (@p st.shared) — no regs[rd] read-back
        ui = nui; gi += live ? wt : 0;               // a predicated-off body uop retires 0 guest instrs
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = regs[i];
    // Resume pc: a missed JALR target (→ host translates it), else the current uop's pc, else halt.
    g.pc = (resume_pc != HALT_BIT) ? resume_pc : (((int32_t)ui >= 0) ? __ldg(&uop2pc[ui]) : HALT_BIT);
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
static int        g_nuops   = 0;
static int        g_pc2words = 0;       // length of pc2uop[] = translated code words (NOT total memory words)
static uint32_t   g_base    = 0;
static unsigned long long* g_ret = nullptr;  // core-0 retired guest-instruction count (verify gate)
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

API int cuda_rv32i_init(int nCores, unsigned int memBytes) {
    g_ncores = nCores;
    cudaError_t e;
    if ((e = cudaMallocManaged(&g_state, (size_t)nCores * sizeof(CoreState))) != cudaSuccess) return (int)e;
    if ((e = cudaMalloc(&g_mem, (size_t)nCores * memBytes)) != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    cudaMemset(g_mem, 0, (size_t)nCores * memBytes);
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

API int cuda_rv32i_step_all(int budget) {
    if (g_ncores <= 0) return 0;
    // Small (2-warp) blocks: at modest core counts this spreads work across many
    // more SMs than a 256-thread block (which would pile onto a handful of SMs and
    // leave the rest idle) — the interpreter is latency-bound, so SM coverage wins.
    int block = g_ncores < 64 ? g_ncores : 64;
    int grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 33 * sizeof(uint32_t);
    rv32i_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget);
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

#if RVCUD_BITFIELD
    // ── BFE — srli rt,rs,k ; andi rd,rt,mask  (rt dead after) → rd = (rs>>k)&mask (one uop) ──
    if (OPC(w)==0x13 && ((img[w]>>12)&7)==5 && ((img[w]>>25)&0x7F)==0          // srli (logical, f7==0)
        && w+1<N && !B.leader[w+1] && OPC(w+1)==0x13 && ((img[w+1]>>12)&7)==7) {  // andi
        uint32_t i1=img[w], i2=img[w+1];
        uint32_t rt=(i1>>7)&0x1F, rs=(i1>>15)&0x1F, k=(i1>>20)&0x1F;
        uint32_t rd=(i2>>7)&0x1F, ra=(i2>>15)&0x1F, mask=rv_iimm(i2);           // andi imm sign-extends to 32 b
        bool rtdead = (rd==rt) || !(B.liveout[w+1] & (1u<<rt));                 // the shift temp must not escape
        if (rt!=0 && ra==rt && rs!=rt && rtdead && mask <= 0xFFFFu) {           // real positive mask (excludes andi -1 = mv)
            w0 = RCW0(RC_BFE, rd, rs, 0, 0, 0, RP_UNC, 0);
            w1 = (k & 0x1F) | (mask << 5); weight = 2; tgt = RC_NOTGT;          // pack k[4:0] | mask<<5
            return 2;
        }
    }
#endif

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

// MEMORY-COPY FUSION. A byte/half/word copy step `lb rt,0(src); sb rt,0(dst); addi src,Ks; addi dst,Kd`
// (the four in ANY order — clang interleaves the pointer bumps differently for horizontal memcpy vs the
// vertical column blit) collapses to ONE uop: mem[dst]=mem[src]; src+=Ks; dst+=Kd. Requires offset 0 on
// both, rt dead after the store, and src/dst/rt distinct. Replaces 4 rv32i instrs/iter with 1 — Doom's
// hottest code is exactly these copy loops (span blits + column blits). Strides fit signed 16 bits.
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
// is the per-uop bottleneck). Pervasive in Doom (every pointer+index access). Correct-by-construction:
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
// post-increment style. Saves a uop per iteration in copy/scan/blit loops, which dominate Doom's hot
// path (column blits, memcpy). Requires offset 0 (so w1 carries the increment), the addi to target the
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

// SOFTWARE-MULTIPLY LOOP → MULLOOP. clang's rv32i shift-add multiply (the renderer's FixedMul) is a
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

#if RVCUD_LOADFWD
// STATIC REDUNDANT-LOAD ELIMINATION (#5). A load whose (base,offset,width,sign=f3) matches an EARLIER
// load in the SAME basic block — with the base register unchanged and NO store between (conservative
// alias kill) and the earlier dest still holding its value — is replaced by a register copy (ADDC rd,rA,0),
// dropping the memory op. Sound: mem[base+off] is provably unchanged across the window, so the second
// load reads identical bits → gate-identical (weight 1, same as the load it replaces). clang -O2 already
// elides most of these, so EV is low; the flag exists to MEASURE the Doom hit-rate. Default OFF.
static int rvcud_try_loadfwd(RvcudBuild& B, int w, uint32_t& ui) {
    const uint32_t* img = B.img;
    uint32_t i0 = img[w]; if ((i0 & 0x7F) != 0x03) return 0;
    uint32_t f3 = (i0>>12)&7; if (!(f3==0||f3==1||f3==2||f3==4||f3==5)) return 0;
    uint32_t rd = (i0>>7)&0x1F, rb = (i0>>15)&0x1F, off = rv_iimm(i0);
    if (rd == 0 || B.leader[w]) return 0;                 // don't forward into a block entry (no dominance)
    uint32_t redef = 0;                                   // registers written strictly between j and w
    for (int j = w-1; j >= 0; j--) {
        uint32_t in = img[j], op = in & 0x7F, jrd = (in>>7)&0x1F;
        if (op == 0x23) return 0;                         // a store between → may alias the address → bail
        if (op == 0x03 && ((in>>12)&7)==f3 && ((in>>15)&0x1F)==rb && rv_iimm(in)==off) {   // matching producer load
            uint32_t rA = jrd;
            if (rA != 0 && rA != rb && !((redef>>rA)&1u) && !((redef>>rb)&1u)) {            // rA still holds it; base stable
                B.pc2uop[w] = ui;
                B.w0.push_back(RCW0(RC_ADDC, rd, rA, 0, 0, 0, RP_UNC, 0));
                B.w1.push_back(0); B.uw.push_back(1);
                B.u2pc.push_back(B.base + (uint32_t)w*4); B.tgt.push_back(RC_NOTGT); ui++;
                return 1;
            }
        }
        if (op==0x6F || op==0x67 || op==0x73) return 0;   // call/jump/system between → not straight-line
        if (jrd) { if (jrd == rb) return 0; redef |= (1u << jrd); }   // base reg changed before w → no stable producer
        if (B.leader[j]) break;                           // reached this block's start
    }
    return 0;
}
#endif

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
        int consumed = rvcud_try_mulloop(B, w, ui);                // shift-add software-multiply loop → 1 hw multiply
        if (consumed == 0) consumed = rvcud_try_divloop(B, w, ui);  // bit-serial software-divide loop → 1 hw divide
        if (consumed == 0) consumed = rvcud_try_incbr(B, w, ui);   // counted-loop addi+branch → INCBR
        if (consumed == 0) consumed = rvcud_try_ifconv(B, w, ui);  // if-conversion (short forward branch → predicated)
        if (consumed == 0) consumed = rvcud_try_affine(B, w, ui);  // affine ×const fold → MULADD/MULC + ADD chain
        if (consumed == 0) consumed = rvcud_try_memset(B, w, ui);    // whole byte-fill (memset) loop → 1 native uop
        if (consumed == 0) consumed = rvcud_try_copyloop(B, w, ui);  // whole byte-memcpy loop → 1 word-widened uop
        if (consumed == 0) consumed = rvcud_try_copy(B, w, ui);     // lb;sb;addi;addi memory-copy step → 1 uop
        if (consumed == 0) consumed = rvcud_try_ldxs(B, w, ui);     // slli + add + load → scaled-indexed load (1 uop)
        if (consumed == 0) consumed = rvcud_try_ldx(B, w, ui);      // add + load/store → indexed mem op (1 uop)
        if (consumed == 0) consumed = rvcud_try_postinc(B, w, ui);  // load/store + base post-increment → 1 uop
#if RVCUD_LOADFWD
        if (consumed == 0) consumed = rvcud_try_loadfwd(B, w, ui);  // redundant load → register copy (no memory op)
#endif
        if (consumed == 0) {
            B.pc2uop[w] = ui;
            uint32_t a0, a1, t; uint8_t wt;
            consumed = rvcud_try_fuse(B, w, a0, a1, wt, t);   // ≥1; swallowed words stay BADUOP (LEA/CONST/XSH)
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
        else if (cls == RC_COPYLOOP || cls == RC_MULLOOP || cls == RC_MEMSET) {   // preserve hi byte (reg); bake fall-through into low 24
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x00FFFFFFu) : 0x00FFFFFFu;
            B.w1[i] = (B.w1[i] & 0xFF000000u) | idx;
        }
        else if (cls == RC_DIVLOOP) {                                // preserve bits[31:22] (regs); bake fall-through into low 22
            uint32_t tw = B.tgt[i];
            uint32_t idx = ((int)tw < N && B.pc2uop[tw] != RC_BADUOP) ? (B.pc2uop[tw] & 0x003FFFFFu) : 0x003FFFFFu;
            B.w1[i] = (B.w1[i] & 0xFFC00000u) | idx;
        }
    }
}

static void rvcud_free() {
    if (g_uops)   { cudaFree(g_uops);   g_uops   = nullptr; }
    if (g_uw)     { cudaFree(g_uw);     g_uw     = nullptr; }
    if (g_pc2uop) { cudaFree(g_pc2uop); g_pc2uop = nullptr; }
    if (g_uop2pc) { cudaFree(g_uop2pc); g_uop2pc = nullptr; }
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
    cudaMemcpy(g_uops,   uops.data(),     (size_t)g_nuops * sizeof(uint2), cudaMemcpyHostToDevice);
    cudaMemcpy(g_uw,     g_uwh.data(),    (size_t)g_nuops,                cudaMemcpyHostToDevice);
    cudaMemcpy(g_uop2pc, g_u2pch.data(),  (size_t)g_nuops * 4,            cudaMemcpyHostToDevice);
    cudaMemcpy(g_pc2uop, g_pc2uop_h.data(), (size_t)N * 4,               cudaMemcpyHostToDevice);
    return (int)cudaDeviceSynchronize();
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
    if (!g_ret) cudaMallocManaged(&g_ret, sizeof(unsigned long long));
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
    for (int guard = 0; guard < 1 << 20; guard++) {
        int rem = budget - (int)total;
        if (rem <= 0) break;
        rvcud_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_uops, g_uw, g_pc2uop, g_uop2pc,
                                             g_ncores, g_pc2words, g_base, rem, g_ret);
        cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
        if (le != cudaSuccess) return (int)le;
        if (se != cudaSuccess) return (int)se;
        total += (long long)*g_ret;                  // core 0 retired this launch
        uint32_t pc = g_state[0].pc;                 // managed memory → host-readable
        if (pc & 0x80000000u) break;                 // halted (illegal instr / guest done)
        int w = (pc >= g_base) ? (int)((pc - g_base) >> 2) : -1;
        if (w >= 0 && w < g_pc2words && g_pc2uop_h[w] == RC_BADUOP) {
            if (rvcud_translate_miss(pc)) continue;   // translated the missed block → resume
        }
        break;                                        // normal: budget spent at a translated pc
    }
    *g_ret = (unsigned long long)total;               // report TOTAL retired (verify gate reads this)
    return 0;
}

// Guest-instructions retired by core 0 in the last cuda_rvcud_step_all (≈ budget + overshoot,
// since a fused uop retires several). The verify gate runs rv32i for exactly this many.
API unsigned long long cuda_rvcud_retired() { return g_ret ? *g_ret : 0ull; }
API unsigned long long cuda_rvcud_iters() { unsigned long long h=0; cudaMemcpyFromSymbol(&h,g_dev_iters,sizeof(h)); return h; }

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
    if (g_ret) { cudaFree(g_ret); g_ret = nullptr; }
    if (g_mem)  { cudaFree(g_mem);  g_mem  = nullptr; }
    if (g_state) { cudaFree(g_state); g_state = nullptr; }
    g_ncores = 0;
}
