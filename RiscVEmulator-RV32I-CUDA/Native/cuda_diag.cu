// cuda_diag.cu — single-thread GPU performance microbenchmark.
//
// Answers: is the RV32I interpreter's ~2.6 MIPS/core a fundamental single-warp
// latency wall, or a pathology (idle GPU clocks / managed-memory PCIe access)?
//
//   nvcc -O3 -std=c++20 -arch=sm_86 -o cuda_diag.exe cuda_diag.cu && cuda_diag.exe

#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

#define CK(x) do{ cudaError_t e=(x); if(e){printf("CUDA err %d (%s) @ %d\n",(int)e,cudaGetErrorString(e),__LINE__); return 1;} }while(0)

// Dependent ALU chain — no memory. Measures raw single-thread issue rate +
// achieved clock (via clock64()).
__global__ void k_compute(uint64_t* out, long long iters) {
    uint32_t a = threadIdx.x + 1u, b = 7u;
    long long c0 = clock64();
    for (long long i = 0; i < iters; i++) {
        a = a * 1664525u + 1013904223u;   // each iter depends on the last
        b ^= a; b = (b << 1) | (b >> 31);
    }
    long long c1 = clock64();
    out[0] = (uint64_t)a + b;
    out[1] = (uint64_t)(c1 - c0);
}

// Dependent pointer-chase — one load per iter, each address from the previous
// load. Measures unhidden dependent-load latency in clocks.
__global__ void k_chase(const uint32_t* mem, uint64_t* out, long long iters) {
    uint32_t idx = 0;
    long long c0 = clock64();
    for (long long i = 0; i < iters; i++) idx = mem[idx];
    long long c1 = clock64();
    out[0] = idx;
    out[1] = (uint64_t)(c1 - c0);
}

// Dependent SHARED-memory chase — models the interpreter's register-file
// access (regs[] live in shared, dynamically indexed). Measures unhidden
// shared-load latency for one warp lane.
__global__ void k_shared_chase(uint64_t* out, long long iters) {
    __shared__ uint32_t s[64];
    for (int i = 0; i < 64; i++) s[i] = (uint32_t)((i * 7 + 13) & 63);
    uint32_t idx = 0;
    long long c0 = clock64();
    for (long long i = 0; i < iters; i++) idx = s[idx & 63];   // dependent shared load
    long long c1 = clock64();
    out[0] = idx; out[1] = (uint64_t)(c1 - c0);
}

// "JIT output" model — the guest's xorshift loop run as NATIVE GPU code (what a
// translator would emit: guest regs in real registers, no fetch/decode/dispatch).
// Each inner iter ≈ 8 RV32I guest instructions (3×(shift+xor) + counter + branch),
// so we can compare guest-instr/s against the ~2.7 MIPS interpreter.
__global__ void k_native(uint64_t* out, long long outer) {
    uint32_t x = 12345u;
    long long c0 = clock64();
    for (long long o = 0; o < outer; o++)
        for (int i = 0; i < 4096; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    long long c1 = clock64();
    out[0] = x; out[1] = (uint64_t)(c1 - c0);
}

static double wall(cudaEvent_t a, cudaEvent_t b){ float ms=0; cudaEventElapsedTime(&ms,a,b); return ms/1e3; }

int main() {
    int dev=0; cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, dev));
    int clkKHz=0, memKHz=0;
    cudaDeviceGetAttribute(&clkKHz, cudaDevAttrClockRate, dev);
    cudaDeviceGetAttribute(&memKHz, cudaDevAttrMemoryClockRate, dev);
    printf("GPU: %s  SMs=%d  max SM clock=%.0f MHz  mem clock=%.0f MHz\n\n",
           p.name, p.multiProcessorCount, clkKHz/1000.0, memKHz/1000.0);

    uint64_t *out; CK(cudaMallocManaged(&out, 2*sizeof(uint64_t)));
    cudaEvent_t e0,e1; cudaEventCreate(&e0); cudaEventCreate(&e1);

    // ── 1) Raw compute rate + achieved clock ──
    {
        long long iters = 200LL*1000*1000;
        k_compute<<<1,1>>>(out, 1000);            // warm up
        CK(cudaDeviceSynchronize());
        cudaEventRecord(e0);
        k_compute<<<1,1>>>(out, iters);
        cudaEventRecord(e1); CK(cudaDeviceSynchronize());
        double s = wall(e0,e1);
        double cyc = (double)out[1];
        printf("[compute] %lld iters in %.4f s  ->  %.2f Giter/s\n", iters, s, iters/s/1e9);
        printf("          %.2f clocks/iter   achieved clock = %.0f MHz%s\n",
               cyc/iters, cyc/s/1e6,
               (cyc/s/1e6 < 0.6*clkKHz/1000.0) ? "  <-- GPU NOT at boost (idle clocks)!" : "");
    }

    // ── 1a) "JIT output": native single-thread guest-equiv throughput ──
    {
        long long outer = 40000;                 // 40000*4096 inner iters
        k_native<<<1,1>>>(out, 100); CK(cudaDeviceSynchronize());
        cudaEventRecord(e0);
        k_native<<<1,1>>>(out, outer);
        cudaEventRecord(e1); CK(cudaDeviceSynchronize());
        double s = wall(e0,e1);
        double guestInstr = (double)outer * 4096.0 * 8.0;   // ~8 RV32I/inner iter
        printf("[native ] %.0f M guest-instr-equiv in %.3f s  ->  %.1f MIPS single-thread\n",
               guestInstr/1e6, s, guestInstr/s/1e6);
        printf("          (vs ~2.7 MIPS interpreter → the JIT's single-core ceiling)\n");
    }

    // ── 1b) Dependent SHARED-memory access latency (register-file model) ──
    {
        long long iters = 50LL*1000*1000;
        k_shared_chase<<<1,1>>>(out, 1000); CK(cudaDeviceSynchronize());
        cudaEventRecord(e0);
        k_shared_chase<<<1,1>>>(out, iters);
        cudaEventRecord(e1); CK(cudaDeviceSynchronize());
        double s = wall(e0,e1); double cyc = (double)out[1];
        printf("[shared ] %.2f clocks/dependent-load   %.2f Mload/s   (one warp lane)\n",
               cyc/iters, iters/s/1e6);
    }

    // ── 2) Dependent-load latency: device vs managed memory ──
    const int N = 1<<20;                          // 4 MiB cycle (> L2 on most data)
    long long chaseIters = 20LL*1000*1000;
    auto run_chase = [&](uint32_t* mem, const char* tag)->int{
        // stride permutation so the chase can't be prefetched and stays a cycle
        for (int i=0;i<N;i++) mem[i] = (uint32_t)((i + 131071) % N);
        k_chase<<<1,1>>>(mem, out, 1000); CK(cudaDeviceSynchronize());   // warm
        cudaEventRecord(e0);
        k_chase<<<1,1>>>(mem, out, chaseIters);
        cudaEventRecord(e1); CK(cudaDeviceSynchronize());
        double s = wall(e0,e1);
        double cyc = (double)out[1];
        printf("[chase %-8s] %.2f clocks/load   %.2f Mload/s   (%.2f s)\n",
               tag, cyc/chaseIters, chaseIters/s/1e6, s);
        return 0;
    };
    {
        uint32_t* dev; CK(cudaMalloc(&dev, (size_t)N*4));
        uint32_t* tmp = (uint32_t*)malloc((size_t)N*4);
        for (int i=0;i<N;i++) tmp[i]=(uint32_t)((i+131071)%N);
        CK(cudaMemcpy(dev, tmp, (size_t)N*4, cudaMemcpyHostToDevice));
        // re-run with device ptr (init done on host, copied)
        k_chase<<<1,1>>>(dev, out, 1000); CK(cudaDeviceSynchronize());
        cudaEventRecord(e0); k_chase<<<1,1>>>(dev, out, chaseIters); cudaEventRecord(e1);
        CK(cudaDeviceSynchronize());
        double s=wall(e0,e1), cyc=(double)out[1];
        printf("[chase device  ] %.2f clocks/load   %.2f Mload/s   (%.2f s)\n",
               cyc/chaseIters, chaseIters/s/1e6, s);
        free(tmp); cudaFree(dev);
    }
    {
        uint32_t* man; CK(cudaMallocManaged(&man, (size_t)N*4));
        if (run_chase(man, "managed")) return 1;
        // prefetch to device and retry (if supported on this OS)
        cudaMemLocation loc; loc.type = cudaMemLocationTypeDevice; loc.id = dev;
        cudaError_t pe = cudaMemPrefetchAsync(man, (size_t)N*4, loc, 0, (cudaStream_t)0);
        if (pe==cudaSuccess) { cudaDeviceSynchronize(); run_chase(man, "man+pref"); }
        else printf("[chase man+pref ] prefetch unsupported (CUDA %d %s)\n",(int)pe,cudaGetErrorString(pe));
        cudaFree(man);
    }
    printf("\nRule of thumb: a single warp can't hide latency, so MIPS ~= clock / (clocks-per-instr).\n");
    printf("If achieved clock is idle and managed-load latency >> device-load latency, those are the wins.\n");
    return 0;
}
