// rv32i_cuda.cu — RV32IMA core ported to CUDA. Windows / nvcc.
//
// GPU port of Native/rv32i_core.cpp. The base CPU + trap unit are ported and
// run __device__; the only structural change vs the host build is the memory
// seam: there is no Windows VEH, so mem_read/mem_write do an explicit
// `if (range)` dispatch. RAM, the trap page, the framebuffer and the audio PCM
// buffer are plain (managed) memory; guarded peripherals (UART, keyboard,
// mouse, RTC, MIDI, display/audio control, CLINT, host-exit) get simple
// device-side semantics against a per-core managed `Periph` page that the host
// reconciles between launches.
//
// ISA: RV32I + M (MUL/MULH*/DIV*/REM*) + A (LR/SC/AMO*.W). M/A let guests use
// native mul/div/atomics instead of slow software libcalls — a large speedup
// for arithmetic-heavy guests. (No F: software float via the guest runtime.)
//
// PERFORMANCE: a single GPU thread cannot hide memory latency, so the #1 cost
// is touching the register file in *global* memory every instruction. The
// kernel therefore hoists the hot CPU state (regs[32], pc, priv, pending,
// halted) into a thread-local `Hart` at launch, runs the whole budget on it
// (hot path force-inlined → state stays in registers / L1 local memory), and
// writes it back to the managed CoreState at the end. The cold trap CSR-ish
// store (soft_csr) is reached through CoreState::soft_csr (a pointer into a
// separately allocated backing store — see PICO note in rv32i_jit_shared.cuh).
//
// Execution model: one GPU thread per core (__launch_bounds__(1,1)). All shared
// buffers are CUDA managed memory; Windows/WDDM forbids host access while a
// kernel runs, so the host only touches them between launches (batch-sync).
//
// Decoders stay `constexpr`, called from __device__ via --expt-relaxed-constexpr.

#include <cstdint>
#include <cstring>
#include <cuda_runtime.h>
#include <cuda_pipeline.h>   // __pipeline_memcpy_async / commit / wait_prior (sm_80+)

// Shared memory model + trap unit. This header holds the CoreState/CoreMem/
// Periph/Hart structs, the memory-map constants, ld_le/st_le, the full
// mem_read/mem_write dispatch, the MMIO device semantics, the trap unit
// (do_trap/trap_return/trap_system/check_interrupts), the SEC_WORDS/SEC_BYTES
// prefetch geometry, and the jit_*/div helper inlines. The runtime GPU JIT
// module (Core/Cuda/RvJitRuntime.cs) #includes the SAME header so its kernels
// share byte-identical layout + semantics with this interpreter and can run
// against the very same managed CoreState[]/CoreMem[] this DLL allocates.
#include "rv32i_jit_shared.cuh"

// NOTE: the PREDICT fast-path flag `g_fastpath_on` is DEFINED in the shared
// header (inside #ifndef RVJIT_MEM_ONLY, next to cpu_step) so each module that
// compiles cpu_step gets its own copy. This TU just toggles it from the host
// via cuda_rv32i_set_fastpath → cudaMemcpyToSymbol(g_fastpath_on, …).

// ── Instruction prefetch window (Tier 3, single-/few-guest latency) ──────
// A single warp cannot hide the ~530-cycle dependent-load latency of an
// instruction fetch. The prefetch path keeps a double-buffered window of two
// 32-instruction sections (64 instructions = 256 B) per thread in shared
// memory: the current section is executed from on-chip shared memory (~30 cyc),
// while the NEXT section is cp.async-copied from the shared RO code image in
// the background, overlapping the copy with execution. In-window jumps (loops
// up to a section) hit instantly with no fetch at all. Enabled per-launch via
// cuda_rv32i_set_prefetch; off by default (the many-core throughput path wants
// the shared budget for occupancy, not for windows).
// (SEC_WORDS / SEC_BYTES are defined in rv32i_jit_shared.cuh.)

// ════════════════════════════════════════════════════════════════════
// PART 1 — RV32IMA CPU + PART 2 — TRAP UNIT
// ════════════════════════════════════════════════════════════════════
// cpu_step, the j_imm/b_imm/i_imm/s_imm decoders, the EXC_*/CpuException
// definitions, jit_interp_step, and the full trap unit (do_trap/trap_return/
// trap_system/check_interrupts + all STATUS_*/CAUSE_*/IE_*/FRAME_*/
// PV_RESUME_GATEWAY constants) now live in rv32i_jit_shared.cuh, so the JIT
// module shares them byte-for-byte. The interpreter's prefetch-window fetch
// and the kernel below are the only interpreter-specific pieces left here.

// ── Instruction fetch ────────────────────────────────────────────────
// cp.async-copy SEC_WORDS instructions from the shared RO code image into a
// shared-memory section. 4-byte granularity keeps it alignment-free (the
// section buffers are only 4-byte aligned within the per-thread stride).
static __device__ __forceinline__ void prefetch_section(uint32_t* dst, CoreMem& m, uint32_t vbase) {
    const uint8_t* src = m.code + (vbase - m.code_lo);
    #pragma unroll
    for (uint32_t i = 0; i < SEC_WORDS; i++)
        __pipeline_memcpy_async(dst + i, src + i * 4u, 4);
    __pipeline_commit();
}

// Fetch the instruction at cpu.pc. PF=false: plain global read (the original
// path). PF=true: serve from the double-buffered shared window, overlapping the
// next section's copy with execution and serving in-window jumps with no fetch.
template<bool PF>
static __device__ __forceinline__ uint32_t fetch(Hart& c, CoreMem& m) {
    if constexpr (!PF) {
        return mem_read<uint32_t>(c, m, c.pc);
    } else {
        uint32_t pc = c.pc;
        // Code not in the shared image (e.g. JIT'd code running from RAM, or no
        // shared image installed): fall back to a normal load.
        if (!(pc - m.code_lo < m.code_hi - m.code_lo)) return mem_read<uint32_t>(c, m, pc);

        uint32_t ts = pc & ~(SEC_BYTES - 1u);                 // section base
        int k = (c.sbase[0] == ts) ? 0 : (c.sbase[1] == ts) ? 1 : -1;
        if (k < 0) {                                          // window miss (cold / far jump)
            int v = (c.sbase[0] <= c.sbase[1]) ? 0 : 1;       // evict the lower-addressed half
            __pipeline_wait_prior(0); c.pend = 0;             // drain any in-flight copy first
            prefetch_section(c.sec + v * SEC_WORDS, m, ts);
            __pipeline_wait_prior(0);                         // this one we need now
            c.sbase[v] = ts; k = v;
        } else if (c.pend & (1u << k)) {                      // section arriving — wait for it
            __pipeline_wait_prior(0); c.pend = 0;
        }
        // Kick the next sequential section into the other half, overlapping the
        // ~32 instructions we are about to execute (only if it isn't already there).
        uint32_t ns = ts + SEC_BYTES;
        if (ns - m.code_lo < m.code_hi - m.code_lo) {
            int nk = 1 - k;
            if (c.sbase[nk] != ns) {
                prefetch_section(c.sec + nk * SEC_WORDS, m, ns);
                c.sbase[nk] = ns; c.pend = (1u << nk);
            }
        }
        return c.sec[k * SEC_WORDS + ((pc - ts) >> 2)];
    }
}

// BOOL branch-eliminator A/B switch. Default ON (branchless hot path). Define
// RVCUDA_BRANCHLESS=0 at compile time to fall back to the switch-based baseline
// for a head-to-head measurement (both paths are bit-exact — see PERF_BOOL.md).
#ifndef RVCUDA_BRANCHLESS
#define RVCUDA_BRANCHLESS 1
#endif

template<bool PF>
static __device__ __forceinline__ void do_step(Hart& cpu, CoreMem& mm) {
    if (check_interrupts(cpu, mm)) return;
    if (cpu.pc == PV_RESUME_GATEWAY) { trap_return(cpu, mm); return; }
    CpuException e = cpu_step<RVCUDA_BRANCHLESS != 0>(cpu, mm, fetch<PF>(cpu, mm));
    if (e.kind == EXC_SYSTEM)       trap_system(cpu, mm, e.instr);
    else if (e.kind == EXC_ILLEGAL) do_trap(cpu, mm, CAUSE_ILLEGAL, e.instr);
}

// ════════════════════════════════════════════════════════════════════
// EXPERIMENT — function-pointer dispatch (one __device__ fn per opcode,
// table in __shared__) instead of the switch. Tests whether ptxas was
// predicating the opcode switch (executing multiple arms). Indexed by the
// RISC-V major opcode (instr>>2)&0x1F (32 entries). Each handler returns the
// next pc + any exception; the common wrapper does the x0 sink + pc update +
// trap dispatch (matching cpu_step). Toggle with cuda_rv32i_set_fpdispatch.
// ════════════════════════════════════════════════════════════════════
struct StepRes { uint32_t nextpc; uint32_t kind; uint32_t einstr; };
typedef StepRes (*OpFn)(Hart&, CoreMem&, uint32_t);

static __device__ StepRes hIllegal(Hart& c, CoreMem&, uint32_t instr){ return { c.pc+4, EXC_ILLEGAL, instr }; }
static __device__ StepRes hLUI  (Hart& c, CoreMem&, uint32_t instr){ c.regs[(instr>>7)&0x1F] = instr & 0xFFFFF000u;            return { c.pc+4, EXC_NONE, 0 }; }
static __device__ StepRes hAUIPC(Hart& c, CoreMem&, uint32_t instr){ c.regs[(instr>>7)&0x1F] = c.pc + (instr & 0xFFFFF000u);   return { c.pc+4, EXC_NONE, 0 }; }
static __device__ StepRes hJAL  (Hart& c, CoreMem&, uint32_t instr){ c.regs[(instr>>7)&0x1F] = c.pc+4;                         return { c.pc + j_imm(instr), EXC_NONE, 0 }; }
static __device__ StepRes hJALR (Hart& c, CoreMem&, uint32_t instr){ uint32_t t=(uint32_t)((int32_t)c.regs[(instr>>15)&0x1F] + i_imm(instr)) & ~1u; c.regs[(instr>>7)&0x1F]=c.pc+4; return { t, EXC_NONE, 0 }; }
static __device__ StepRes hFENCE(Hart& c, CoreMem&, uint32_t)      { return { c.pc+4, EXC_NONE, 0 }; }
static __device__ StepRes hSYSTEM(Hart& c, CoreMem&, uint32_t instr){ return { c.pc+4, EXC_SYSTEM, instr }; }

static __device__ StepRes hBRANCH(Hart& c, CoreMem&, uint32_t instr){
    uint32_t f3=(instr>>12)&7; uint32_t u1=c.regs[(instr>>15)&0x1F], u2=c.regs[(instr>>20)&0x1F]; int32_t s1=u1,s2=u2; int taken=0;
    switch (f3){ case 0:taken=u1==u2;break; case 1:taken=u1!=u2;break; case 4:taken=s1<s2;break; case 5:taken=s1>=s2;break; case 6:taken=u1<u2;break; case 7:taken=u1>=u2;break; }
    return { taken ? c.pc + b_imm(instr) : c.pc+4, EXC_NONE, 0 };
}
static __device__ StepRes hLOAD(Hart& c, CoreMem& m, uint32_t instr){
    int rd=(instr>>7)&0x1F; uint32_t f3=(instr>>12)&7; uint32_t a=(uint32_t)((int32_t)c.regs[(instr>>15)&0x1F]+i_imm(instr));
    switch (f3){ case 0:c.regs[rd]=(uint32_t)(int8_t)mem_read<uint8_t>(c,m,a);break; case 1:c.regs[rd]=(uint32_t)(int16_t)mem_read<uint16_t>(c,m,a);break;
                 case 2:c.regs[rd]=mem_read<uint32_t>(c,m,a);break; case 4:c.regs[rd]=mem_read<uint8_t>(c,m,a);break; case 5:c.regs[rd]=mem_read<uint16_t>(c,m,a);break; }
    return { c.pc+4, EXC_NONE, 0 };
}
static __device__ StepRes hSTORE(Hart& c, CoreMem& m, uint32_t instr){
    uint32_t f3=(instr>>12)&7; uint32_t a=(uint32_t)((int32_t)c.regs[(instr>>15)&0x1F]+s_imm(instr)); uint32_t u2=c.regs[(instr>>20)&0x1F];
    switch (f3){ case 0:mem_write<uint8_t>(c,m,a,(uint8_t)u2);break; case 1:mem_write<uint16_t>(c,m,a,(uint16_t)u2);break; case 2:mem_write<uint32_t>(c,m,a,u2);break; }
    return { c.pc+4, EXC_NONE, 0 };
}
static __device__ StepRes hOPIMM(Hart& c, CoreMem&, uint32_t instr){
    int rd=(instr>>7)&0x1F; uint32_t f3=(instr>>12)&7, f7=(instr>>25)&0x7F; uint32_t u1=c.regs[(instr>>15)&0x1F]; int32_t s1=u1; int32_t imm=i_imm(instr); int sh=(instr>>20)&0x1F;
    if ((f3==1&&f7!=0)||(f3==5&&f7!=0&&f7!=0x20)) return { c.pc+4, EXC_ILLEGAL, instr };
    uint32_t r=0;
    switch (f3){ case 0:r=(uint32_t)(s1+imm);break; case 1:r=u1<<sh;break; case 2:r=s1<imm?1u:0u;break; case 3:r=u1<(uint32_t)imm?1u:0u;break;
                 case 4:r=u1^(uint32_t)imm;break; case 5:r=f7==0x20?(uint32_t)(s1>>sh):u1>>sh;break; case 6:r=u1|(uint32_t)imm;break; case 7:r=u1&(uint32_t)imm;break; }
    c.regs[rd]=r; return { c.pc+4, EXC_NONE, 0 };
}
static __device__ StepRes hOP(Hart& c, CoreMem&, uint32_t instr){
    int rd=(instr>>7)&0x1F; uint32_t f3=(instr>>12)&7, f7=(instr>>25)&0x7F; uint32_t u1=c.regs[(instr>>15)&0x1F], u2=c.regs[(instr>>20)&0x1F]; int32_t s1=u1,s2=u2; uint32_t r;
    if (f7==0x01){
        switch (f3){ case 0:r=(uint32_t)(u1*u2);break; case 1:r=(uint32_t)(((int64_t)s1*(int64_t)s2)>>32);break; case 2:r=(uint32_t)(((int64_t)s1*(int64_t)(uint64_t)u2)>>32);break;
                     case 3:r=(uint32_t)(((uint64_t)u1*(uint64_t)u2)>>32);break;
                     case 4:r=(s2==0)?0xFFFFFFFFu:(s1==(int32_t)0x80000000&&s2==-1)?0x80000000u:(uint32_t)(s1/s2);break;
                     case 5:r=(u2==0)?0xFFFFFFFFu:(u1/u2);break;
                     case 6:r=(s2==0)?u1:(s1==(int32_t)0x80000000&&s2==-1)?0u:(uint32_t)(s1%s2);break;
                     case 7:r=(u2==0)?u1:(u1%u2);break; default:r=0;break; }
    } else if (f7!=0x00 && !(f7==0x20 && (f3==0||f3==5))) { return { c.pc+4, EXC_ILLEGAL, instr };
    } else { int sh=s2&0x1F;
        switch (f3){ case 0:r=f7==0x20?(uint32_t)(s1-s2):(uint32_t)(s1+s2);break; case 1:r=u1<<sh;break; case 2:r=s1<s2?1u:0u;break; case 3:r=u1<u2?1u:0u;break;
                     case 4:r=u1^u2;break; case 5:r=f7==0x20?(uint32_t)(s1>>sh):u1>>sh;break; case 6:r=u1|u2;break; default:r=u1&u2;break; }
    }
    c.regs[rd]=r; return { c.pc+4, EXC_NONE, 0 };
}
static __device__ StepRes hAMO(Hart& c, CoreMem& m, uint32_t instr){
    int rd=(instr>>7)&0x1F; uint32_t f3=(instr>>12)&7;
    if (f3!=2) return { c.pc+4, EXC_ILLEGAL, instr };
    uint32_t addr=c.regs[(instr>>15)&0x1F], u2=c.regs[(instr>>20)&0x1F], f5=(instr>>27)&0x1F;
    if (f5==0x02){ c.regs[rd]=mem_read<uint32_t>(c,m,addr); }
    else if (f5==0x03){ mem_write<uint32_t>(c,m,addr,u2); c.regs[rd]=0; }
    else { uint32_t t=mem_read<uint32_t>(c,m,addr), res;
        switch (f5){ case 0x00:res=t+u2;break; case 0x01:res=u2;break; case 0x04:res=t^u2;break; case 0x08:res=t|u2;break; case 0x0C:res=t&u2;break;
                     case 0x10:res=((int32_t)t<(int32_t)u2)?t:u2;break; case 0x14:res=((int32_t)t>(int32_t)u2)?t:u2;break; case 0x18:res=(t<u2)?t:u2;break; case 0x1C:res=(t>u2)?t:u2;break;
                     default: return { c.pc+4, EXC_ILLEGAL, instr }; }
        mem_write<uint32_t>(c,m,addr,res); c.regs[rd]=t; }
    return { c.pc+4, EXC_NONE, 0 };
}

// Fill a 32-entry table by major opcode (instr>>2)&0x1F.
static __device__ __forceinline__ void fp_fill(OpFn* h){
    #pragma unroll
    for (int i=0;i<32;i++) h[i]=hIllegal;
    h[0]=hLOAD; h[3]=hFENCE; h[4]=hOPIMM; h[5]=hAUIPC; h[8]=hSTORE; h[11]=hAMO;
    h[12]=hOP;  h[13]=hLUI;  h[24]=hBRANCH; h[25]=hJALR; h[27]=hJAL; h[28]=hSYSTEM;
}

static __device__ __forceinline__ void do_step_fp(Hart& cpu, CoreMem& mm, OpFn* h){
    if (check_interrupts(cpu, mm)) return;
    if (cpu.pc == PV_RESUME_GATEWAY) { trap_return(cpu, mm); return; }
    uint32_t instr = mem_read<uint32_t>(cpu, mm, cpu.pc);
    StepRes rs = h[(instr>>2)&0x1F](cpu, mm, instr);     // indirect call (shared-resident fn ptr)
    if (rs.kind == EXC_NONE)        { cpu.regs[0]=0; cpu.pc = rs.nextpc; }
    else if (rs.kind == EXC_SYSTEM) trap_system(cpu, mm, rs.einstr);
    else                            do_trap(cpu, mm, CAUSE_ILLEGAL, rs.einstr);
}

// ════════════════════════════════════════════════════════════════════
// Kernel — one core per thread, packed BLOCK_DIM cores/block (32/warp).
// ════════════════════════════════════════════════════════════════════

// One core per thread, packed `g_block` cores/block (32 lanes/warp). A single
// warp can't hide the ~530-cycle dependent-load latency of an instruction
// fetch, so the whole game is OCCUPANCY: pack threads to keep many warps
// resident per SM. BLOCK_DIM=1 caps at the ~16-blocks/SM hardware limit (~16
// warps/SM, 1 lane each = a few % of the machine); packing to 256 lifts that
// toward the 48-warp/SM ceiling. Packing only pays once fetches coalesce
// (Tier 1b shared code) so the 32 lanes' fetches collapse to one transaction.
// MAX_BLOCK bounds the launch-bounds hint; the actual blockDim is g_block.
static constexpr int MAX_BLOCK = 256;

// Per-thread shared stride: regs (33 words, bank-conflict-free) plus, on the
// prefetch path, the two-section instruction window (2*SEC_WORDS). The total
// stride stays coprime-ish with 32 so reg/window accesses don't bank-conflict
// across lanes (gcd(33,32)=1; gcd(33+64,32)=gcd(97,32)=1).
// MAXB is the launch-bounds register budget knob. The packed throughput path
// uses MAXB=256 (tight register cap → high occupancy). The single-/few-guest
// path uses a small MAXB so ptxas may use many more registers and stop spilling
// the interpreter's hot state to local memory — at block=256 the cap forces 40
// regs and the interpreter spills (72 B baseline, 340 B with the window), which
// is the real single-core cost, not fetch latency.
template<bool PF, int MAXB>
__global__ void __launch_bounds__(MAXB)
rv32i_kernel(CoreState* st, CoreMem* mm, int ncores, long long budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    CoreMem    m = mm[id];   // local copy → CoreMem fields live in registers, not re-read from managed global

    constexpr int STRIDE = PF ? (33 + 2 * (int)SEC_WORDS) : 33;
    extern __shared__ uint32_t s_state[];  // size = blockDim.x * STRIDE words
    Hart h;
    h.regs = &s_state[threadIdx.x * STRIDE];
    if constexpr (PF) {
        h.sec = h.regs + 33;
        h.sbase[0] = 0xFFFFFFFFu; h.sbase[1] = 0xFFFFFFFFu; h.pend = 0;
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) h.regs[i] = g.regs[i];
    h.pc = g.pc; h.priv = g.priv; h.pending = g.pending;
    h.halted = g.halted; h.exitcode = g.exitcode; h.exited = g.exited;
    h.g = &g;

    for (long long i = 0; i < budget && !h.halted; i++) do_step<PF>(h, m);

    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = h.regs[i];
    g.pc = h.pc; g.priv = h.priv; g.pending = h.pending;
    g.halted = h.halted; g.exitcode = h.exitcode; g.exited = h.exited;
}

// EXPERIMENT kernel: same as rv32i_kernel<false,MAXB> but dispatches via the
// shared function-pointer table (one __device__ fn per opcode) instead of the
// switch in cpu_step. The 32-entry table is a per-block __shared__ array filled
// once by thread 0; the regfile uses the same 33-word dynamic-shared stride.
template<int MAXB>
__global__ void __launch_bounds__(MAXB)
rv32i_kernel_fp(CoreState* st, CoreMem* mm, int ncores, long long budget) {
    __shared__ OpFn s_h[32];                 // function-pointer table in shared memory
    if (threadIdx.x == 0) fp_fill(s_h);
    __syncthreads();

    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    CoreMem    m = mm[id];   // local copy → CoreMem fields in registers

    extern __shared__ uint32_t s_state[];    // blockDim.x * 33 words (regfile)
    Hart h;
    h.regs = &s_state[threadIdx.x * 33];
    #pragma unroll
    for (int i = 0; i < 32; i++) h.regs[i] = g.regs[i];
    h.pc = g.pc; h.priv = g.priv; h.pending = g.pending;
    h.halted = g.halted; h.exitcode = g.exitcode; h.exited = g.exited;
    h.g = &g;

    for (long long i = 0; i < budget && !h.halted; i++) do_step_fp(h, m, s_h);

    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = h.regs[i];
    g.pc = h.pc; g.priv = h.priv; g.pending = h.pending;
    g.halted = h.halted; g.exitcode = h.exitcode; g.exited = h.exited;
}

// ════════════════════════════════════════════════════════════════════
// Host C-ABI
// ════════════════════════════════════════════════════════════════════

#define API extern "C" __declspec(dllexport)

static CoreState* g_state  = nullptr;
static CoreMem*   g_mem    = nullptr;
static int        g_ncores = 0;
static uint8_t*   g_code    = nullptr;  // shared RO code image (Tier 1b)
static size_t     g_code_cap = 0;       // padded byte capacity of g_code
static int        g_block   = MAX_BLOCK;// cores per block (occupancy / packing knob)
static int        g_prefetch = 0;       // 1 → double-buffered shared instruction window
static int        g_fpdispatch = 0;     // 1 → function-pointer opcode dispatch (experiment)
static int        g_l2advise = 1;       // 1 → memory-residency hints (Mott; default ON)
static int        g_l2advise_dirty = 1; // 1 → (re)apply residency hints on next step_all

// ── L2-resident working set (Mott) ───────────────────────────────────────
// A single GPU lane is dependent-load-latency-bound: the ~530-cycle stall is a
// DRAM round-trip on every instruction fetch / register touch. The placement
// attack is to keep the whole working set device-resident so accesses hit L2
// (~200 cyc) instead of fault-migrating from host DRAM over PCIe.
// cudaMallocManaged pages start with NO physical residency — the first device
// touch faults them in over the bus, and under memory pressure they can migrate
// back to host. We therefore:
//   • tag the shared RO code image ReadMostly + PreferredLocation=device, so it
//     is pinned/duplicated device-side (the guest never writes its own .text);
//   • set PreferredLocation=device on each core's writable RAM/trap/periph; and
//   • cudaMemPrefetchAsync every buffer to the device before the first launch,
// so the working set is materialized in device memory up front and stays there
// instead of fault-migrating mid-run. This is PURELY a placement hint: it can
// never change a load/store result, so RV32IMA stays bit-for-bit identical.
// Failures are swallowed — advice is best-effort by design.
static void l2_advise_buf(void* p, size_t bytes, int dev, bool readMostly) {
    if (!p || bytes == 0) return;
    cudaMemLocation loc; loc.type = cudaMemLocationTypeDevice; loc.id = dev;
    if (readMostly) cudaMemAdvise(p, bytes, cudaMemAdviseSetReadMostly, loc);
    cudaMemAdvise(p, bytes, cudaMemAdviseSetPreferredLocation, loc);
    cudaMemPrefetchAsync(p, bytes, loc, 0, (cudaStream_t)0);
}

// Apply residency hints to the whole per-core CPU working set + shared code
// image. No-op when g_l2advise==0, so the baseline path keeps the untouched
// original allocation behaviour, bit-for-bit.
static void l2_advise_all() {
    if (!g_l2advise || !g_mem) return;
    int dev = 0;
    if (cudaGetDevice(&dev) != cudaSuccess) { cudaGetLastError(); return; }
    // cudaMemAdvise / cudaMemPrefetchAsync on managed memory require
    // concurrentManagedAccess. Windows/WDDM reports 0 → those calls fail with
    // cudaErrorInvalidDevice (101) and leave a STICKY error that the next kernel
    // launch's cudaGetLastError() would surface as a bogus launch failure. On
    // WDDM the working set is already fully device-resident for the duration of
    // a launch (the host cannot touch managed memory while a kernel runs), so
    // the hints are a no-op anyway — skip them and clear any latent error.
    int cma = 0;
    if (cudaDeviceGetAttribute(&cma, cudaDevAttrConcurrentManagedAccess, dev) != cudaSuccess || !cma) {
        cudaGetLastError();   // swallow any sticky best-effort-hint error
        return;
    }
    l2_advise_buf(g_state, (size_t)g_ncores * sizeof(CoreState), dev, false);
    l2_advise_buf(g_mem,   (size_t)g_ncores * sizeof(CoreMem),   dev, false);
    for (int i = 0; i < g_ncores; i++) {
        l2_advise_buf(g_mem[i].ram,  g_mem[i].ram_size, dev, false);
        l2_advise_buf(g_mem[i].trap, TRAP_PAGE_SIZE,    dev, false);
        l2_advise_buf(g_mem[i].per,  sizeof(Periph),    dev, false);
        // fb/pcm are large and host-reconciled every launch; leave them unhinted
        // so they don't evict the hot CPU working set from device residency.
    }
    if (g_code) l2_advise_buf(g_code, g_code_cap, dev, /*readMostly=*/true);
    cudaDeviceSynchronize();
    cudaGetLastError();   // hints are best-effort: never let one leak into the next launch
}

// PICO (state-minimizer) — soft-CSR backing-store allocation strategy. Both
// strategies are BIT-EXACT (CSR[fn] reads/writes the identical word); they
// differ only in *where* the SOFT_CSR_COUNT-word file per core lives:
//   g_pico==1 (default): ONE contiguous slab of nCores*SOFT_CSR_COUNT words.
//       sizeof(CoreState) is now ~64 B (the old inline 16 KiB array became an
//       8 B pointer), so the densely-packed g_state[] array is ~256× smaller —
//       far cheaper per-launch state region and L2-resident. The CSR slab is
//       cold (compute guests never touch it) and pages in lazily on demand.
//   g_pico==0 (baseline): per-core cudaMallocManaged buffers, reproducing the
//       original layout's per-core allocation footprint for A/B comparison.
// g_csr holds the slab (pico) so shutdown can free it in one call.
static int        g_pico  = 1;
static uint32_t*  g_csr   = nullptr;     // pico: contiguous CSR slab; baseline: nullptr

API int cuda_rv32i_init(int nCores, unsigned int ramSize, unsigned int fbW, unsigned int fbH,
                        unsigned int pcmBytes) {
    g_ncores = nCores;
    // NOTE: do NOT force max shared-memory carveout — the interpreter is
    // latency-bound and relies on a large L1 to cache instruction fetches and
    // guest data. Maxing shared starves L1 and is ~40x slower. 33 KB of shared
    // fits comfortably in the driver-default carveout, which keeps L1 large.
    cudaError_t e;
    if ((e = cudaMallocManaged(&g_state, (size_t)nCores * sizeof(CoreState))) != cudaSuccess) return (int)e;
    if ((e = cudaMallocManaged(&g_mem,   (size_t)nCores * sizeof(CoreMem)))   != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    memset(g_mem,   0, (size_t)nCores * sizeof(CoreMem));

    // Soft-CSR backing store. PICO: one contiguous slab (the per-core file is a
    // slice of it); baseline: a private buffer per core (allocated in the loop).
    // Either way each core gets exactly SOFT_CSR_COUNT zeroed words, so a CSR
    // read before any CSR write returns 0 just as the old inline array did.
    g_csr = nullptr;
    if (g_pico) {
        if ((e = cudaMallocManaged(&g_csr, (size_t)nCores * SOFT_CSR_COUNT * sizeof(uint32_t))) != cudaSuccess) return (int)e;
        memset(g_csr, 0, (size_t)nCores * SOFT_CSR_COUNT * sizeof(uint32_t));
    }

    unsigned int fbBytes = fbW * fbH * 4u;
    for (int i = 0; i < nCores; i++) {
        uint8_t* ram = nullptr; uint8_t* trap = nullptr; Periph* per = nullptr;
        uint8_t* fb = nullptr;  uint8_t* pcm = nullptr;
        if ((e = cudaMallocManaged(&ram,  ramSize))         != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&trap, TRAP_PAGE_SIZE))  != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&per,  sizeof(Periph)))  != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&fb,   fbBytes))         != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&pcm,  pcmBytes))        != cudaSuccess) return (int)e;
        memset(ram, 0, ramSize); memset(trap, 0, TRAP_PAGE_SIZE); memset(per, 0, sizeof(Periph));
        memset(fb, 0, fbBytes);  memset(pcm, 0, pcmBytes);
        // Wire the per-core CSR file (slab slice in pico, private buffer in baseline).
        if (g_pico) {
            g_state[i].soft_csr = g_csr + (size_t)i * SOFT_CSR_COUNT;
        } else {
            uint32_t* csr = nullptr;
            if ((e = cudaMallocManaged(&csr, SOFT_CSR_COUNT * sizeof(uint32_t))) != cudaSuccess) return (int)e;
            memset(csr, 0, SOFT_CSR_COUNT * sizeof(uint32_t));
            g_state[i].soft_csr = csr;
        }
        g_mem[i].ram = ram; g_mem[i].trap = trap; g_mem[i].per = per;
        g_mem[i].fb = fb;   g_mem[i].pcm = pcm;
        g_mem[i].ram_size = ramSize; g_mem[i].fb_bytes = fbBytes; g_mem[i].pcm_bytes = pcmBytes;
        g_mem[i].fb_w = fbW; g_mem[i].fb_h = fbH;
        g_state[i].priv = PRIV_M;
        per->au_rate = 22050; per->au_chan = 1; per->au_bits = 16;
    }
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}

// Managed base pointers for the per-core arrays. These are the crux of the
// JIT integration: the runtime-built JIT DLL (rv32i_jit_guest.dll) is a
// separate module, but it launches kernels against the SAME managed arrays
// this DLL allocated. Both DLLs share the process's CUDA primary context, so a
// cudaMallocManaged pointer minted here is fully valid in a kernel launched
// there. The JIT module's init takes these two pointers + ncores and never
// allocates its own state — peripheral reconcile (UART/FB/kbd/…) therefore
// keeps working between interpreter and JIT launches interchangeably.
API void* cuda_rv32i_state_ptr() { return g_state; }
API void* cuda_rv32i_mem_ptr()   { return g_mem; }
API int   cuda_rv32i_ncores()    { return g_ncores; }

API void*        cuda_rv32i_ram_ptr (int core) { return g_mem[core].ram; }
API unsigned int cuda_rv32i_ram_size(int core) { return g_mem[core].ram_size; }
API void*        cuda_rv32i_fb_ptr  (int core) { return g_mem[core].fb; }
API unsigned int cuda_rv32i_fb_bytes(int core) { return g_mem[core].fb_bytes; }
API void*        cuda_rv32i_pcm_ptr (int core) { return g_mem[core].pcm; }
API unsigned int cuda_rv32i_pcm_bytes(int core){ return g_mem[core].pcm_bytes; }

API void cuda_rv32i_set_reg  (int core, int i, unsigned int v) { if (i) g_state[core].regs[i & 31] = v; }
API void cuda_rv32i_set_entry(int core, unsigned int pc)       { g_state[core].pc = pc; }
API unsigned int cuda_rv32i_get_pc (int core) { return g_state[core].pc; }
API int  cuda_rv32i_is_halted(int core)       { return g_state[core].halted; }
API void cuda_rv32i_set_halted(int core, int v){ g_state[core].halted = v; }
API int  cuda_rv32i_exitcode (int core)       { return g_state[core].exitcode; }

API void cuda_rv32i_set_mtip(int core, int level) {
    if (level) g_state[core].pending |= PIN_MTIP; else g_state[core].pending &= ~PIN_MTIP;
}
API void cuda_rv32i_set_meip(int core, int level) {
    if (level) g_state[core].pending |= PIN_MEIP; else g_state[core].pending &= ~PIN_MEIP;
}

// Tier 1b — install the shared read-only code image (the guest's RO PT_LOAD
// span). `data`/`len` are the bytes at guest VA `lo`. Pass len==0 to disable.
// Must be called after init (writes every core's CoreMem) and before stepping.
API int cuda_rv32i_set_code(const void* data, unsigned int lo, unsigned int len) {
    if (g_code) { cudaFree(g_code); g_code = nullptr; g_code_cap = 0; }
    if (len == 0) {
        for (int i = 0; i < g_ncores; i++) { g_mem[i].code = nullptr; g_mem[i].code_lo = 0; g_mem[i].code_hi = 0; }
        return 0;
    }
    cudaError_t e;
    // Pad capacity to a whole section so the prefetch window can copy a full
    // SEC_BYTES block past the logical end without reading out of bounds.
    size_t cap = (len + SEC_BYTES - 1u) & ~((size_t)SEC_BYTES - 1u);
    if ((e = cudaMallocManaged(&g_code, cap)) != cudaSuccess) return (int)e;
    g_code_cap = cap;
    memset(g_code, 0, cap);
    memcpy(g_code, data, len);
    for (int i = 0; i < g_ncores; i++) { g_mem[i].code = g_code; g_mem[i].code_lo = lo; g_mem[i].code_hi = lo + len; }
    g_l2advise_dirty = 1;   // new code image: (re)hint it on the next step_all
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}

// Occupancy knob: cores packed per block (1..MAX_BLOCK). 1 = one core/warp.
API void cuda_rv32i_set_block(int b) { if (b >= 1 && b <= MAX_BLOCK) g_block = b; }

// Tier 3: enable the double-buffered shared instruction window (best for single-
// /few-guest latency). Off = the original global-fetch path (best occupancy).
API void cuda_rv32i_set_prefetch(int on) { g_prefetch = on ? 1 : 0; }
// EXPERIMENT: function-pointer opcode dispatch instead of the switch.
API void cuda_rv32i_set_fpdispatch(int on) { g_fpdispatch = on ? 1 : 0; }
// PREDICT: frequency-biased predicated fast path in cpu_step (default ON).
// Writes the device-side g_fastpath_on symbol. OFF reproduces the switch
// bit-for-bit; the orchestrator A/Bs this against ON for MIPS + verify.
API int cuda_rv32i_set_fastpath(int on) {
    int v = on ? 1 : 0;
    cudaError_t e = cudaMemcpyToSymbol(g_fastpath_on, &v, sizeof(int));
    return (int)e;
}

// PICO (state-minimizer): select the soft-CSR backing-store allocation strategy.
// Default ON (slab). MUST be called BEFORE cuda_rv32i_init — it only governs how
// init lays out the CSR file; both layouts are bit-exact. Pass 0 to force the
// baseline per-core layout for an A/B comparison.
API void cuda_rv32i_set_pico(int on) { g_pico = on ? 1 : 0; }
API int  cuda_rv32i_get_pico()       { return g_pico; }
// Resident-footprint introspection: bytes of densely-packed per-core state
// (g_state[]) the kernel syncs at launch/exit. The smaller this is, the more
// cores co-reside at a fixed VRAM budget. Useful for the CudaBench A/B printout.
API unsigned int cuda_rv32i_corestate_bytes() { return (unsigned int)sizeof(CoreState); }
// Mott — L2-resident working set. ON (default): hint the CPU working set
// (state/mem/per-core ram/trap/periph + shared code) device-preferred and
// prefetch it to the GPU before launches. OFF: original allocation behaviour
// (baseline). Placement only — results are bit-for-bit identical either way.
// Hints are re-applied lazily on the next step_all (set_code / set_l2advise
// dirty the flag) so a toggle or a new code image always takes effect.
API void cuda_rv32i_set_l2advise(int on) { g_l2advise = on ? 1 : 0; g_l2advise_dirty = 1; }
API int  cuda_rv32i_get_l2advise()       { return g_l2advise; }

API int cuda_rv32i_step_all(long long budget) {
    if (g_ncores <= 0) return 0;
    // Mott — materialize the working set device-resident before the launch.
    // Applied lazily (only when dirtied by set_l2advise / set_code) so the
    // common many-launch run pays the advise/prefetch cost just once.
    if (g_l2advise && g_l2advise_dirty) { l2_advise_all(); g_l2advise_dirty = 0; }
    int block = g_block;
    if (block > g_ncores) block = g_ncores;
    if (block < 1) block = 1;
    int    grid   = (g_ncores + block - 1) / block;
    int    stride = g_prefetch ? (33 + 2 * (int)SEC_WORDS) : 33;
    size_t shmem  = (size_t)block * stride * sizeof(uint32_t);

    // EXPERIMENT path: shared function-pointer opcode dispatch (regfile stride 33).
    if (g_fpdispatch) {
        size_t fshmem = (size_t)block * 33 * sizeof(uint32_t);
        if (block <= 32) rv32i_kernel_fp<32 ><<<grid, block, fshmem>>>(g_state, g_mem, g_ncores, budget);
        else             rv32i_kernel_fp<256><<<grid, block, fshmem>>>(g_state, g_mem, g_ncores, budget);
        cudaError_t lef = cudaGetLastError();
        cudaError_t sef = cudaDeviceSynchronize();
        return lef != cudaSuccess ? (int)lef : (int)sef;
    }
    // Register-budget tiers via launch-bounds. Smaller MAXB ⇒ ptxas may use
    // more registers ⇒ fewer/no spills, but lower occupancy. Pick the tightest
    // tier whose bound covers the block size:
    //   block ≤ 32  → MAXB=32  (84 regs, 0 spill)  — single/few-guest latency
    //   block ≤ 128 → MAXB=128 (no spill, mid occ) — balanced throughput
    //   else        → MAXB=256 (40 regs, spills)   — max occupancy
    // The sweep measures which wins for aggregate throughput (spill DRAM traffic
    // vs occupancy). Only the packed prefetch window can exceed 48 KB shared.
    if (g_prefetch && block > 128 && shmem > 48u * 1024u)
        cudaFuncSetAttribute((const void*)rv32i_kernel<true, MAX_BLOCK>,
                             cudaFuncAttributeMaxDynamicSharedMemorySize, (int)shmem);
    #define LAUNCH(MB) do { \
        if (g_prefetch) rv32i_kernel<true,  MB><<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget); \
        else            rv32i_kernel<false, MB><<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget); \
    } while (0)
    if      (block <= 32)  LAUNCH(32);
    else if (block <= 128) LAUNCH(128);
    else                   LAUNCH(MAX_BLOCK);
    #undef LAUNCH
    cudaError_t le = cudaGetLastError();          // launch-config errors land here
    cudaError_t se = cudaDeviceSynchronize();     // execution errors land here
    return le != cudaSuccess ? (int)le : (int)se;
}

API int cuda_rv32i_uart_drain(int core, unsigned char* dst, int maxlen) {
    Periph* p = g_mem[core].per;
    int n = 0;
    while (p->tx_tail != p->tx_head && n < maxlen) { dst[n++] = p->tx[p->tx_tail & TXM]; p->tx_tail++; }
    return n;
}
API void cuda_rv32i_uart_feed(int core, const unsigned char* src, int len) {
    Periph* p = g_mem[core].per;
    for (int i = 0; i < len; i++) { p->rx[p->rx_head & RXM] = src[i]; p->rx_head++; }
}

API void cuda_rv32i_kbd_feed(int core, unsigned int entry) {
    Periph* p = g_mem[core].per;
    p->kbd[p->kbd_head & KBM] = entry; p->kbd_head++;
}
API void cuda_rv32i_kbd_set_mod(int core, unsigned int mod) { g_mem[core].per->kbd_mod = mod; }

API void cuda_rv32i_mouse_feed(int core, int dx, int dy, unsigned int buttons) {
    Periph* p = g_mem[core].per;
    p->mouse_dx += dx; p->mouse_dy += dy; p->mouse_buttons = buttons;
    p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u;
}

API int cuda_rv32i_midi_drain(int core, unsigned int* dst, int maxlen) {
    Periph* p = g_mem[core].per;
    int n = 0;
    while (p->midi_tail != p->midi_head && n < maxlen) { dst[n++] = p->midi[p->midi_tail & MDM]; p->midi_tail++; }
    return n;
}

API void cuda_rv32i_audio_snapshot(int core, unsigned int* out8) {
    Periph* p = g_mem[core].per;
    out8[0]=p->au_ctrl; out8[1]=p->au_rate; out8[2]=p->au_chan; out8[3]=p->au_bits;
    out8[4]=p->au_bufstart; out8[5]=p->au_buflen; out8[6]=p->au_pos; out8[7]=p->au_wrgen;
}
API void cuda_rv32i_audio_ack(int core) { g_mem[core].per->au_ctrl = 0; }

API unsigned int cuda_rv32i_display_take_vsync(int core) {
    Periph* p = g_mem[core].per; unsigned int v = p->dc_vsync; p->dc_vsync = 0; return v;
}
API unsigned int cuda_rv32i_display_fbaddr(int core) { return g_mem[core].per->dc_fbaddr; }
API unsigned int cuda_rv32i_display_mode  (int core) { return g_mem[core].per->dc_mode; }

API void cuda_rv32i_set_time(int core, unsigned int usLo, unsigned int usHi,
                             unsigned int msLo, unsigned int msHi,
                             unsigned int epLo, unsigned int epHi,
                             unsigned int sec, unsigned int subus) {
    Periph* p = g_mem[core].per;
    p->rtc_us_lo=usLo; p->rtc_us_hi=usHi; p->rtc_ms_lo=msLo; p->rtc_ms_hi=msHi;
    p->rtc_epoch_lo=epLo; p->rtc_epoch_hi=epHi; p->rtc_sec=sec; p->rtc_subus=subus;
}
API void cuda_rv32i_set_mtime(int core, unsigned int lo, unsigned int hi) {
    Periph* p = g_mem[core].per; p->mtime_lo = lo; p->mtime_hi = hi;
}
API unsigned long long cuda_rv32i_mtimecmp(int core) {
    Periph* p = g_mem[core].per;
    return ((unsigned long long)p->mtimecmp_hi << 32) | p->mtimecmp_lo;
}

API void cuda_rv32i_shutdown() {
    if (g_mem) {
        for (int i = 0; i < g_ncores; i++) {
            cudaFree(g_mem[i].ram); cudaFree(g_mem[i].trap); cudaFree(g_mem[i].per);
            cudaFree(g_mem[i].fb);  cudaFree(g_mem[i].pcm);
        }
        cudaFree(g_mem); g_mem = nullptr;
    }
    // Free the soft-CSR backing: one slab in pico, per-core buffers in baseline.
    if (g_csr) { cudaFree(g_csr); g_csr = nullptr; }
    else if (g_state) {
        for (int i = 0; i < g_ncores; i++) cudaFree(g_state[i].soft_csr);
    }
    if (g_code) { cudaFree(g_code); g_code = nullptr; g_code_cap = 0; }
    cudaFree(g_state); g_state = nullptr;
    g_ncores = 0;
    g_l2advise_dirty = 1;   // next init starts fresh
}
