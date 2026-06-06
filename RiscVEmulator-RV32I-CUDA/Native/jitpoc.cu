// jitpoc.cu — trace-JIT PoC. Proves (1) regs-in-registers beats the interpreter,
// and (2) the runtime PTX/dispatch machinery: emit PTX for a trace from uops at
// runtime, JIT-assemble via the driver API (cuModuleLoadDataEx = ptxas, no cicc),
// run it, match the interpreter. Same dependent RV32I-style loop, three ways,
// single guest <<<1,1>>>.
//   nvcc -O3 -arch=sm_86 -lcuda -o jitpoc.exe jitpoc.cu && ./jitpoc.exe
#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <cuda_runtime.h>
#include <cuda.h>

struct Uop { uint8_t op, rd, rs1, rs2; int32_t imm; };   // op: 0 add,1 xor,2 sll,3 addi,5 or

static __device__ __forceinline__ uint32_t alu(int op, uint32_t a, uint32_t b) {
    switch (op) { case 0: return a + b; case 1: return a ^ b; case 2: return a << (b & 31);
                  case 5: return a | b; default: return a + b; }
}

// (A) interpreter: regs in shared memory, dynamically indexed (like our core).
__global__ void interp_kernel(const Uop* __restrict__ prog, int n, long iters, uint32_t* out) {
    __shared__ uint32_t regs[32];
    for (int i = 0; i < 32; i++) regs[i] = i + 1;
    for (long it = 0; it < iters; it++)
        for (int k = 0; k < n; k++) {
            Uop u = prog[k];
            uint32_t a = regs[u.rs1];
            uint32_t b = (u.op == 3) ? (uint32_t)u.imm : regs[u.rs2];
            regs[u.rd] = alu(u.op, a, b);
        }
    for (int i = 0; i < 32; i++) out[i] = regs[i];
}

// ── runtime PTX codegen: emit a trace (load regs → loop body → store regs) ──
static std::string gen_ptx(const Uop* p, int n) {
    std::ostringstream o;
    o << ".version 7.8\n.target sm_86\n.address_size 64\n"
      << ".visible .entry jit_trace(.param .u64 pr, .param .u64 pit) {\n"
      << "  .reg .pred %p<2>;\n  .reg .u32 %x<32>;\n  .reg .u32 %t<2>;\n  .reg .u64 %rd<4>;\n"
      << "  ld.param.u64 %rd1, [pr];\n  ld.param.u64 %rd2, [pit];\n  cvta.to.global.u64 %rd1, %rd1;\n";
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
        }
    }
    o << "  add.u64 %rd3, %rd3, 1;\n  bra $L;\n$D:\n";
    for (int i = 0; i < 32; i++) o << "  st.global.u32 [%rd1+" << i*4 << "], %x" << i << ";\n";
    o << "  ret;\n}\n";
    return o.str();
}

int main() {
    Uop hprog[8] = {
        {0,5,5,6,0}, {1,6,6,5,0}, {0,7,7,5,0}, {2,8,8,6,0},
        {1,9,9,7,0}, {0,10,10,8,0}, {5,11,11,9,0}, {0,5,5,11,0},
    };
    const int N = 8; const long ITERS = 2'000'000;
    Uop* dprog; cudaMalloc(&dprog, sizeof(hprog)); cudaMemcpy(dprog, hprog, sizeof(hprog), cudaMemcpyHostToDevice);
    uint32_t *oa, *ob, *oc; cudaMalloc(&oa,128); cudaMalloc(&ob,128); cudaMalloc(&oc,128);
    uint32_t init[32]; for (int i=0;i<32;i++) init[i]=i+1;

    cudaEvent_t s,e; cudaEventCreate(&s); cudaEventCreate(&e);
    auto timeit = [&](auto launch){ launch(); cudaDeviceSynchronize();
        cudaEventRecord(s); launch(); cudaEventRecord(e); cudaEventSynchronize(e);
        float ms=0; cudaEventElapsedTime(&ms,s,e); return ms; };

    float ai = timeit([&]{ interp_kernel<<<1,1>>>(dprog,N,ITERS,oa); });

    // ── runtime PTX JIT via driver API (validates the machinery) ──
    cudaFree(0);                                   // ensure a context exists
    std::string ptx = gen_ptx(hprog, N);
    CUmodule mod; CUfunction fn;
    char log[8192]; CUjit_option opt[2]={CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
    void* ov[2]={log,(void*)sizeof(log)}; log[0]=0;
    CUresult r = cuModuleLoadDataEx(&mod, ptx.c_str(), 2, opt, ov);
    if (r != CUDA_SUCCESS) { printf("PTX JIT failed (%d):\n%s\nPTX:\n%s\n", r, log, ptx.c_str()); return 1; }
    cuModuleGetFunction(&fn, mod, "jit_trace");
    cudaMemcpy(oc, init, 128, cudaMemcpyHostToDevice);
    long iters = ITERS; CUdeviceptr dRegs = (CUdeviceptr)oc;
    void* args[] = { &dRegs, &iters };
    auto launchPtx = [&]{ cuLaunchKernel(fn, 1,1,1, 1,1,1, 0,0, args, 0); };
    float aj = timeit(launchPtx);

    // correctness: one clean run from the same init (timing above ran warm+timed,
    // and the in-place PTX kernel would otherwise reflect two applications).
    cudaMemcpy(oc, init, 128, cudaMemcpyHostToDevice); launchPtx(); cudaDeviceSynchronize();
    uint32_t ha[32], hc[32];
    cudaMemcpy(ha,oa,128,cudaMemcpyDeviceToHost); cudaMemcpy(hc,oc,128,cudaMemcpyDeviceToHost);
    bool ok=true; for(int i=0;i<32;i++) if(ha[i]!=hc[i]) ok=false;

    double ops=(double)ITERS*N;
    printf("guest instrs: %.0fM  (8-op dependent ALU loop, single thread)\n", ops/1e6);
    printf("  interpreter (regs in shared mem):   %8.2f ms = %7.2f MIPS\n", ai, ops/(ai*1e3));
    printf("  PTX JIT (regs in registers, ptxas): %8.2f ms = %7.2f MIPS\n", aj, ops/(aj*1e3));
    printf("  speedup: %.1fx   (results %s)\n", ai/aj, ok?"MATCH":"DIFFER!!");
    return 0;
}
