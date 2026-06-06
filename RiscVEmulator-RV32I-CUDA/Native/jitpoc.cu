// jitpoc.cu — trace-JIT PoC. Proves (1) regs-in-registers beats the interpreter,
// and (2) the runtime PTX/dispatch machinery: emit PTX for a trace from uops at
// runtime, JIT-assemble via the driver API (cuModuleLoadDataEx = ptxas, no cicc),
// run it, match the interpreter. Single guest <<<1,1>>>. Two regimes:
//   (A) pure dependent ALU loop      — the JIT best case (regs are the bottleneck)
//   (B) Doom-like load/store/ALU mix — the honest case (guest loads are a floor)
//   nvcc -O3 -std=c++17 -arch=sm_86 -lcuda -o jitpoc.exe jitpoc.cu && ./jitpoc.exe
#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <cuda_runtime.h>
#include <cuda.h>

// op: 0 add,1 xor,2 sll,3 addi,5 or,6 load rd=mem[(rs1+imm)&MASK],7 store mem[(rs1+imm)&MASK]=rs2
struct Uop { uint8_t op, rd, rs1, rs2; int32_t imm; };
#define MEMW 4096u            // 16 KB working set (L1/L2-resident, like DOOM hot data)
#define MASK (MEMW - 1u)

static __device__ __forceinline__ uint32_t alu(int op, uint32_t a, uint32_t b) {
    switch (op) { case 0: return a + b; case 1: return a ^ b; case 2: return a << (b & 31);
                  case 5: return a | b; default: return a + b; }
}

// (A/B) interpreter: regs in shared memory, dynamically indexed (our core's model).
__global__ void interp_kernel(const Uop* __restrict__ prog, int n, long iters,
                              uint32_t* mem, uint32_t* out) {
    __shared__ uint32_t regs[32];
    for (int i = 0; i < 32; i++) regs[i] = i + 1;
    for (long it = 0; it < iters; it++)
        for (int k = 0; k < n; k++) {
            Uop u = prog[k];
            if (u.op == 6)      regs[u.rd] = mem[(regs[u.rs1] + u.imm) & MASK];
            else if (u.op == 7) mem[(regs[u.rs1] + u.imm) & MASK] = regs[u.rs2];
            else {
                uint32_t a = regs[u.rs1];
                uint32_t b = (u.op == 3) ? (uint32_t)u.imm : regs[u.rs2];
                regs[u.rd] = alu(u.op, a, b);
            }
        }
    for (int i = 0; i < 32; i++) out[i] = regs[i];
}

// ── runtime PTX codegen: emit a trace (load regs → loop body → store regs) ──
// param0 = &regs[32], param1 = iters, param2 = guest mem base.
static std::string gen_ptx(const Uop* p, int n, bool spill = false) {
    std::ostringstream o;
    o << ".version 7.8\n.target sm_86\n.address_size 64\n"
      << ".visible .entry jit_trace(.param .u64 pr, .param .u64 pit, .param .u64 pm) {\n"
      << "  .reg .pred %p<2>;\n  .reg .u32 %x<32>;\n  .reg .u32 %t<4>;\n  .reg .u64 %rd<8>;\n"
      << "  ld.param.u64 %rd1, [pr];\n  ld.param.u64 %rd2, [pit];\n  ld.param.u64 %rd5, [pm];\n"
      << "  cvta.to.global.u64 %rd1, %rd1;\n  cvta.to.global.u64 %rd5, %rd5;\n";
    for (int i = 0; i < 32; i++) o << "  ld.global.u32 %x" << i << ", [%rd1+" << i*4 << "];\n";
    o << "  mov.u64 %rd3, 0;\n$L:\n  setp.ge.u64 %p1, %rd3, %rd2;\n  @%p1 bra $D;\n";
    for (int k = 0; k < n; k++) {
        int rd=p[k].rd, a=p[k].rs1, b=p[k].rs2; long im=p[k].imm;
        switch (p[k].op) {
            case 0: o << "  add.s32 %x"<<rd<<", %x"<<a<<", %x"<<b<<";\n"; break;
            case 1: o << "  xor.b32 %x"<<rd<<", %x"<<a<<", %x"<<b<<";\n"; break;
            case 2: o << "  and.b32 %t0, %x"<<b<<", 31;\n  shl.b32 %x"<<rd<<", %x"<<a<<", %t0;\n"; break;
            case 5: o << "  or.b32 %x"<<rd<<", %x"<<a<<", %x"<<b<<";\n"; break;
            case 3: o << "  add.s32 %x"<<rd<<", %x"<<a<<", "<<im<<";\n"; break;
            case 6: // rd = mem[(rs1+imm)&MASK]
                o << "  add.s32 %t0, %x"<<a<<", "<<im<<";\n  and.b32 %t0, %t0, "<<MASK<<";\n"
                  << "  mul.wide.u32 %rd6, %t0, 4;\n  add.u64 %rd6, %rd5, %rd6;\n"
                  << "  ld.global.u32 %x"<<rd<<", [%rd6];\n"; break;
            case 7: // mem[(rs1+imm)&MASK] = rs2
                o << "  add.s32 %t0, %x"<<a<<", "<<im<<";\n  and.b32 %t0, %t0, "<<MASK<<";\n"
                  << "  mul.wide.u32 %rd6, %t0, 4;\n  add.u64 %rd6, %rd5, %rd6;\n"
                  << "  st.global.u32 [%rd6], %x"<<b<<";\n"; break;
        }
    }
    if (spill) {  // model a dynamic-dispatch (JALR) boundary: spill+reload all 32 regs per trace
        for (int i = 0; i < 32; i++) o << "  st.global.u32 [%rd1+" << i*4 << "], %x" << i << ";\n";
        for (int i = 0; i < 32; i++) o << "  ld.global.u32 %x" << i << ", [%rd1+" << i*4 << "];\n";
    }
    o << "  add.u64 %rd3, %rd3, 1;\n  bra $L;\n$D:\n";
    for (int i = 0; i < 32; i++) o << "  st.global.u32 [%rd1+" << i*4 << "], %x" << i << ";\n";
    o << "  ret;\n}\n";
    return o.str();
}

struct Bench { const char* name; Uop* hprog; int n; long iters; };

static void run(const char* name, Uop* hprog, int n, long iters, bool spill = false) {
    Uop* dprog; cudaMalloc(&dprog, n*sizeof(Uop)); cudaMemcpy(dprog, hprog, n*sizeof(Uop), cudaMemcpyHostToDevice);
    uint32_t *oa, *oc, *mem; cudaMalloc(&oa,128); cudaMalloc(&oc,128); cudaMalloc(&mem, MEMW*4);
    uint32_t init[32]; for (int i=0;i<32;i++) init[i]=i+1;
    uint32_t hmem[MEMW]; for (uint32_t i=0;i<MEMW;i++) hmem[i] = i*2654435761u + 1u;

    cudaEvent_t s,e; cudaEventCreate(&s); cudaEventCreate(&e);
    auto timeit = [&](auto launch){ launch(); cudaDeviceSynchronize();
        cudaEventRecord(s); launch(); cudaEventRecord(e); cudaEventSynchronize(e);
        float ms=0; cudaEventElapsedTime(&ms,s,e); return ms; };

    cudaMemcpy(mem, hmem, MEMW*4, cudaMemcpyHostToDevice);
    float ai = timeit([&]{ interp_kernel<<<1,1>>>(dprog,n,iters,mem,oa); });
    cudaMemcpy(mem, hmem, MEMW*4, cudaMemcpyHostToDevice);          // re-time from clean mem
    ai = timeit([&]{ interp_kernel<<<1,1>>>(dprog,n,iters,mem,oa); });

    cudaFree(0);
    std::string ptx = gen_ptx(hprog, n, spill);
    CUmodule mod; CUfunction fn;
    char log[8192]; CUjit_option opt[2]={CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
    void* ov[2]={log,(void*)sizeof(log)}; log[0]=0;
    CUresult r = cuModuleLoadDataEx(&mod, ptx.c_str(), 2, opt, ov);
    if (r != CUDA_SUCCESS) { printf("PTX JIT failed (%d):\n%s\nPTX:\n%s\n", r, log, ptx.c_str()); return; }
    cuModuleGetFunction(&fn, mod, "jit_trace");
    long it = iters; CUdeviceptr dRegs=(CUdeviceptr)oc, dMem=(CUdeviceptr)mem;
    void* args[] = { &dRegs, &it, &dMem };
    auto launchPtx = [&]{ cuLaunchKernel(fn, 1,1,1, 1,1,1, 0,0, args, 0); };
    cudaMemcpy(oc, init, 128, cudaMemcpyHostToDevice); cudaMemcpy(mem, hmem, MEMW*4, cudaMemcpyHostToDevice);
    float aj = timeit(launchPtx);

    // correctness: one clean run of each from the same init/mem (timing ran warm+timed).
    cudaMemcpy(oa, init, 128, cudaMemcpyHostToDevice); cudaMemcpy(mem, hmem, MEMW*4, cudaMemcpyHostToDevice);
    interp_kernel<<<1,1>>>(dprog,n,iters,mem,oa); cudaDeviceSynchronize();
    uint32_t ha[32]; cudaMemcpy(ha,oa,128,cudaMemcpyDeviceToHost);
    cudaMemcpy(oc, init, 128, cudaMemcpyHostToDevice); cudaMemcpy(mem, hmem, MEMW*4, cudaMemcpyHostToDevice);
    launchPtx(); cudaDeviceSynchronize();
    uint32_t hc[32]; cudaMemcpy(hc,oc,128,cudaMemcpyDeviceToHost);
    bool ok=true; for(int i=0;i<32;i++) if(ha[i]!=hc[i]) ok=false;

    double ops=(double)iters*n;
    printf("%s — %.0fM guest instrs, single thread <<<1,1>>>\n", name, ops/1e6);
    printf("  interpreter (regs in shared mem):   %8.2f ms = %7.2f MIPS\n", ai, ops/(ai*1e3));
    printf("  PTX JIT (regs in registers, ptxas): %8.2f ms = %7.2f MIPS\n", aj, ops/(aj*1e3));
    printf("  speedup: %.1fx   (results %s)\n\n", ai/aj, ok?"MATCH":"DIFFER!!");
    cudaFree(dprog); cudaFree(oa); cudaFree(oc); cudaFree(mem); cuModuleUnload(mod);
}

int main() {
    // (A) pure dependent ALU loop — JIT best case.
    Uop alu8[8] = {
        {0,5,5,6,0}, {1,6,6,5,0}, {0,7,7,5,0}, {2,8,8,6,0},
        {1,9,9,7,0}, {0,10,10,8,0}, {5,11,11,9,0}, {0,5,5,11,0},
    };
    run("(A) pure dependent ALU loop (8 ops)", alu8, 8, 2'000'000);

    // (B) DOOM-like mix: 3 dependent loads + 1 store + 4 ALU (load-chase, addr depends on
    // previous load → the latency floor both paths share). ~37% load, ~12% store: heavier
    // on memory than real DOOM (28% load / 7% store) so this is a conservative JIT floor.
    Uop mix8[8] = {
        {6,5,5,0,0},     // r5 = mem[r5]
        {3,6,5,0,1},     // r6 = r5 + 1
        {6,7,6,0,0},     // r7 = mem[r6]
        {1,8,7,5,0},     // r8 = r7 ^ r5
        {6,9,8,0,0},     // r9 = mem[r8]
        {0,10,9,7,0},    // r10 = r9 + r7
        {7,0,10,9,0},    // mem[r10] = r9
        {3,5,10,0,7},    // r5 = r10 + 7  (feeds next iter's first load addr)
    };
    run("(B) DOOM-like load/store/ALU mix (8 ops)", mix8, 8, 2'000'000);

    // (C) same mix, but model a DYNAMIC-DISPATCH (JALR) boundary every 8 instrs: spill +
    // reload all 32 regs per trace. This is the pessimistic bound (real DOOM: ~2.5% JALR,
    // most boundaries are static/chainable → no spill). Shows how much dispatch erodes (B).
    run("(C) mix + full 32-reg spill/reload per trace (JALR-dispatch floor)", mix8, 8, 2'000'000, true);
    return 0;
}
