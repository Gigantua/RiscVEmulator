// ptxrun.cu — minimal CUDA Driver-API harness for the RV32→PTX "killer" JIT.
//
// Loads a .ptx string with cuModuleLoadDataEx (the driver JIT-compiles it to
// SASS in-process — no nvcc, no DLL), allocates the guest RAM in device memory,
// launches the translated <<<1,1>>> kernel, times it, and prints the sink word.
// Mirrors the RAM-only memory model of Program.JitTemplate so the result can be
// checked against the interpreter on the same self-halting jit_test guest.
//
// argv: ptxPath ramSize sp entry budget [sinkOff]
#include <cuda.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static const char* es(CUresult r){ const char* s=0; cuGetErrorString(r,&s); return s?s:"?"; }
#define CK(x) do{ CUresult _r=(x); if(_r){ printf("ERR %s: %s\n", #x, es(_r)); return 2; } }while(0)

int main(int argc, char** argv){
    if(argc < 6){ printf("usage: ptxrun ptx ramsz sp entry budget [sinkoff]\n"); return 1; }
    const char* ptxpath = argv[1];
    uint32_t ramsz = (uint32_t)strtoul(argv[2], 0, 0);
    uint32_t sp    = (uint32_t)strtoul(argv[3], 0, 0);
    uint32_t entry = (uint32_t)strtoul(argv[4], 0, 0);
    long long budget = atoll(argv[5]);
    uint32_t sinkoff = argc > 6 ? (uint32_t)strtoul(argv[6], 0, 0) : 0x3000u;

    FILE* fp = fopen(ptxpath, "rb"); if(!fp){ printf("no ptx\n"); return 1; }
    fseek(fp,0,SEEK_END); long n = ftell(fp); fseek(fp,0,SEEK_SET);
    char* ptx = (char*)malloc(n+1); fread(ptx,1,n,fp); ptx[n]=0; fclose(fp);

    CK(cuInit(0));
    CUdevice dev; CK(cuDeviceGet(&dev,0));
    CUcontext ctx; CK(cuDevicePrimaryCtxRetain(&ctx, dev)); CK(cuCtxSetCurrent(ctx));

    // In-process JIT: the driver runs ptxas on the PTX string.
    CUmodule mod; CK(cuModuleLoadDataEx(&mod, ptx, 0, 0, 0));
    CUfunction f; CK(cuModuleGetFunction(&f, mod, "rvjit_run"));

    CUdeviceptr ram, regs, pco, exo;
    CK(cuMemAlloc(&ram, ramsz)); CK(cuMemsetD8(ram,0,ramsz));
    CK(cuMemAlloc(&regs, 32*4));
    CK(cuMemAlloc(&pco, 4)); CK(cuMemAlloc(&exo, 4));
    uint32_t exitbase = 0x40000000u;
    void* args[] = { &ram, &ramsz, &sp, &entry, &budget, &exitbase, &regs, &pco, &exo };

    // warmup
    CK(cuLaunchKernel(f, 1,1,1, 1,1,1, 0, 0, args, 0)); CK(cuCtxSynchronize());
    CK(cuMemsetD8(ram,0,ramsz));

    CUevent e0,e1; cuEventCreate(&e0,0); cuEventCreate(&e1,0);
    cuEventRecord(e0,0);
    CK(cuLaunchKernel(f, 1,1,1, 1,1,1, 0, 0, args, 0));
    cuEventRecord(e1,0); CK(cuCtxSynchronize());
    float ms=0; cuEventElapsedTime(&ms,e0,e1);

    uint32_t sink=0, pc=0, ex=0;
    CK(cuMemcpyDtoH(&sink, ram + sinkoff, 4));
    cuMemcpyDtoH(&pc, pco, 4); cuMemcpyDtoH(&ex, exo, 4);
    printf("JITSINK=0x%08X JITMS=%.4f EXIT=%d PC=0x%X\n", sink, ms, (int)ex, pc);
    return 0;
}
