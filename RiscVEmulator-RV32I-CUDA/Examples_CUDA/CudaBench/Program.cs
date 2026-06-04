using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Cuda;

// ── CUDA throughput + single-core latency benchmark ───────────────────
// A single GPU thread interpreting RV32I is dependent-load-latency-bound: every
// instruction fetch is a load whose result is needed immediately, and a single
// warp exposes the full ~530-cycle latency → ~2-3 MIPS. Two levers:
//   • single-/few-guest LATENCY → the shared double-buffered instruction window
//     (cuda_rv32i_set_prefetch): fetch from on-chip shared memory, overlap the
//     next section's cp.async copy with execution, in-window jumps are free.
//   • many-guest THROUGHPUT → occupancy (pack cores/block) + one shared RO code
//     image (coalesced, L2-resident fetch) so thousands of cores scale.

const string Lib = "rv32i_cuda";
[DllImport(Lib)] static extern int    cuda_rv32i_init(int n, uint ram, uint fbW, uint fbH, uint pcm);
[DllImport(Lib)] static extern IntPtr cuda_rv32i_ram_ptr(int core);
[DllImport(Lib)] static extern void   cuda_rv32i_set_reg(int core, int i, uint v);
[DllImport(Lib)] static extern void   cuda_rv32i_set_entry(int core, uint pc);
[DllImport(Lib)] static extern int    cuda_rv32i_set_code(byte[] data, uint lo, uint len);
[DllImport(Lib)] static extern void   cuda_rv32i_set_block(int b);
[DllImport(Lib)] static extern void   cuda_rv32i_set_prefetch(int on);
[DllImport(Lib)] static extern void   cuda_rv32i_set_fpdispatch(int on);
[DllImport(Lib)] static extern int    cuda_rv32i_step_all(long budget);
[DllImport(Lib)] static extern void   cuda_rv32i_shutdown();

const int  RamBytes = 32 * 1024;     // 32 KiB / core — small so many cores fit VRAM
const uint Sp        = 0x00007F00;
string clang  = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln"))) root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("RiscVEmulator.sln not found"); return 2; }

string buildDir = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);

(byte[] elf, uint entry, uint codeLo, uint codeHi, byte[] codeImg) Build(string name)
{
    string srcPath = Path.Combine(root, "Examples_CUDA", "CudaBench", "Programs", name + ".c");
    string elfPath = Path.Combine(buildDir, name + ".elf");
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var a in new[] { "--target=riscv32-unknown-elf","-march=rv32ima","-mabi=ilp32",
            "-nostdlib","-nostartfiles","-O3","-fno-builtin","-ffreestanding",
            "-fuse-ld=lld","-Wl,-e,_start","-Wl,--image-base=0x1000", srcPath, "-o", elfPath })
        psi.ArgumentList.Add(a);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    if (p.ExitCode != 0) { Console.Error.WriteLine(err); Environment.Exit(2); }
    byte[] elfData = File.ReadAllBytes(elfPath);
    byte[] image = new byte[RamBytes];
    uint e = ElfLoader.Load(elfData, new ArrayBus(image));
    var (lo, hi) = ElfLoader.ReadOnlyCodeSpan(elfData);
    return (elfData, e, lo, hi, image[(int)lo..(int)hi]);
}

Console.WriteLine("Compiling guests (rv32ima)...");
var comp  = Build("compute_guest");
var bench = Build("bench_guest");
Console.WriteLine($"  compute RO code: 0x{comp.codeLo:X5}..0x{comp.codeHi:X5}  ({comp.codeImg.Length} B)");
Console.WriteLine($"  bench   RO code: 0x{bench.codeLo:X5}..0x{bench.codeHi:X5}  ({bench.codeImg.Length} B)\n");

// ── JIT validation: translate the compute guest → CUDA C, nvcc-compile, run
//    one tiny safe launch, check result vs the interpreter + report MIPS. ──
if (args.Contains("--jit"))
{
    var jitg = Build("jit_test");                 // finite, self-halting guest
    long budget = 50_000_000;                     // safety cap (guest halts long before)
    string body = RvJit.TranslateBody(jitg.codeImg, jitg.codeLo, jitg.codeHi, jitg.entry);
    string cu = JitTemplate
        .Replace("/*RAMSIZE*/", RamBytes.ToString())
        .Replace("/*ENTRY*/",   $"0x{jitg.entry:X}u")
        .Replace("/*SP*/",      $"0x{Sp:X}u")
        .Replace("/*SINK*/",    "0x3000u")
        .Replace("/*BODY*/",    body);
    string cuPath  = Path.Combine(buildDir, "jit_compute.cu");
    string exePath = Path.Combine(buildDir, "jit_compute.exe");
    File.WriteAllText(cuPath, cu);
    Console.WriteLine($"  emitted {cuPath} ({cu.Length:N0} bytes, {jitg.codeImg.Length/4} guest instrs)");

    string vc  = @"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat";
    string bat = Path.Combine(buildDir, "build_jit.bat");
    string log = Path.Combine(buildDir, "nvcc.log");
    File.WriteAllText(bat,
        "@echo off\r\n" +
        $"call \"{vc}\" >nul 2>&1\r\n" +
        $"nvcc -O3 -std=c++20 -arch=sm_86 -w -o \"{exePath}\" \"{cuPath}\" > \"{log}\" 2>&1\r\n");
    var bp = new ProcessStartInfo("cmd.exe") { UseShellExecute = false };
    bp.ArgumentList.Add("/c"); bp.ArgumentList.Add(bat);
    var bpp = Process.Start(bp)!; bpp.WaitForExit();
    if (bpp.ExitCode != 0)
    {
        Console.Error.WriteLine("nvcc failed:\n" + (File.Exists(log) ? File.ReadAllText(log) : "(no log)"));
        return 1;
    }

    var rp = new ProcessStartInfo(exePath) { RedirectStandardOutput = true, UseShellExecute = false };
    rp.ArgumentList.Add("5000000");   // safety cap; the guest halts long before
    var rpp = Process.Start(rp)!; string jout = rpp.StandardOutput.ReadToEnd(); rpp.WaitForExit();
    uint jitSink = 0; double jitMs = 0;
    var mj = System.Text.RegularExpressions.Regex.Match(jout, "JITSINK=0x([0-9A-Fa-f]+) JITMS=([0-9.]+)");
    if (mj.Success) { jitSink = Convert.ToUInt32(mj.Groups[1].Value, 16); jitMs = double.Parse(mj.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture); }

    // Interpreter reference: same halting guest, run to halt, timed (one launch).
    var img = new byte[RamBytes];
    ElfLoader.Load(jitg.elf, new ArrayBus(img));
    cuda_rv32i_init(1, (uint)RamBytes, 64, 64, 4096);
    Marshal.Copy(img, 0, cuda_rv32i_ram_ptr(0), img.Length);
    cuda_rv32i_set_reg(0, 2, Sp);
    cuda_rv32i_set_entry(0, jitg.entry);
    cuda_rv32i_set_block(1);
    cuda_rv32i_set_prefetch(0);
    var iw = Stopwatch.StartNew();
    cuda_rv32i_step_all(5_000_000);          // halts at the guest's exit-device write
    iw.Stop();
    double interpMs = iw.Elapsed.TotalMilliseconds;
    byte[] w = new byte[4];
    Marshal.Copy(cuda_rv32i_ram_ptr(0) + 0x3000, w, 0, 4);
    uint refSink = BitConverter.ToUInt32(w);
    cuda_rv32i_shutdown();

    bool ok = refSink == jitSink && refSink != 0;
    Console.WriteLine($"  interpreter: sink=0x{refSink:X8}  {interpMs:F1} ms");
    Console.WriteLine($"  JIT        : sink=0x{jitSink:X8}  {jitMs:F1} ms");
    Console.WriteLine($"  → correctness {(ok ? "MATCH ✓" : "MISMATCH ✗")}   speedup {interpMs / Math.Max(jitMs, 0.001):F1}× over interpreter");
    return ok ? 0 : 1;
}

// Minimal single-launch target for Nsight Compute (one tiny kernel, 1 core,
// 64 KB, 30 k steps) so `ncu --launch-count 1` can attribute single-core stalls.
if (args.Contains("--profile1"))
{
    var img = new byte[RamBytes];
    ElfLoader.Load(comp.elf, new ArrayBus(img));
    cuda_rv32i_init(1, RamBytes, 64, 64, 4096);
    Marshal.Copy(img, 0, cuda_rv32i_ram_ptr(0), img.Length);
    cuda_rv32i_set_reg(0, 2, Sp);
    cuda_rv32i_set_entry(0, comp.entry);
    cuda_rv32i_set_code(comp.codeImg, comp.codeLo, (uint)comp.codeImg.Length);
    cuda_rv32i_set_block(1);
    cuda_rv32i_set_prefetch(0);
    cuda_rv32i_step_all(30_000);   // the one launch ncu profiles
    cuda_rv32i_shutdown();
    Console.WriteLine("profile1 done");
    return 0;
}

// Run one config; returns (aggregate MIPS, verify-word = guest RAM[verifyAddr]).
(double mips, uint verify) Run(in (byte[] elf,uint entry,uint lo,uint hi,byte[] img) g,
                               int cores, int block, bool shared, bool prefetch,
                               long budget, uint verifyAddr)
{
    var image = new byte[RamBytes];
    ElfLoader.Load(g.elf, new ArrayBus(image));
    if (cuda_rv32i_init(cores, RamBytes, 64, 64, 4096) != 0) return (-1, 0);
    for (int c = 0; c < cores; c++)
    {
        Marshal.Copy(image, 0, cuda_rv32i_ram_ptr(c), image.Length);
        cuda_rv32i_set_reg(c, 2, Sp);
        cuda_rv32i_set_entry(c, g.entry);
    }
    if (shared) cuda_rv32i_set_code(g.img, g.lo, (uint)g.img.Length);
    cuda_rv32i_set_block(block);
    cuda_rv32i_set_prefetch(prefetch ? 1 : 0);

    cuda_rv32i_step_all(budget);                 // warm-up
    var sw = Stopwatch.StartNew();
    int iters = 3;
    for (int i = 0; i < iters; i++) cuda_rv32i_step_all(budget);
    sw.Stop();
    uint verify = 0;
    if (verifyAddr != 0)
    {
        byte[] w = new byte[4];
        Marshal.Copy(cuda_rv32i_ram_ptr(0) + (int)verifyAddr, w, 0, 4);
        verify = BitConverter.ToUInt32(w);
    }
    cuda_rv32i_shutdown();
    return ((double)cores * budget * iters / sw.Elapsed.TotalSeconds / 1e6, verify);
}

// Per-launch budgets are kept small (<<2 s of GPU work) so a never-halting
// guest can't trip the WDDM TDR watchdog and lock the machine.
// ── 1) Correctness gate: prefetch ON must match prefetch OFF, bit for bit ──
{
    long b = 300_000;
    var off = Run(bench, 1, 1, true, false, b, 0x4000);
    var on  = Run(bench, 1, 1, true, true,  b, 0x4000);
    bool ok = off.verify == on.verify && off.verify != 0;
    Console.WriteLine($"[correctness] bench guest, {b:N0} steps: prefetch OFF a[0]=0x{off.verify:X8}, " +
                      $"ON a[0]=0x{on.verify:X8}  →  {(ok ? "PASS" : "FAIL")}\n");
    if (!ok) return 1;
}

// ── 2) Single-core latency: prefetch OFF vs ON ──
Console.WriteLine("[single core] 1 core — the instruction-window (prefetch) win");
Console.WriteLine("  guest      prefetch   MIPS/core   speedup");
Console.WriteLine("  ───────    ────────   ─────────   ───────");
foreach (var (g, name) in new[] { (comp, "compute"), (bench, "data") })
{
    double off = Run(g, 1, 1, true, false, 300_000, 0).mips;
    double on  = Run(g, 1, 1, true, true,  300_000, 0).mips;
    Console.WriteLine($"  {name,-7}      off       {off,9:F2}");
    Console.WriteLine($"  {name,-7}      on        {on,9:F2}   {on/off,6:F2}x");
}
Console.WriteLine();

// ── 2b) EXPERIMENT: opcode switch vs shared function-pointer table dispatch ──
Console.WriteLine("[dispatch] compute guest, 1 core — switch vs function-pointer table (shared)");
Console.WriteLine("  mode      MIPS/core");
Console.WriteLine("  ───────   ─────────");
foreach (var (fp, name) in new[] { (0, "switch"), (1, "fn-ptr") })
{
    var image = new byte[RamBytes];
    ElfLoader.Load(comp.elf, new ArrayBus(image));
    cuda_rv32i_init(1, (uint)RamBytes, 64, 64, 4096);
    Marshal.Copy(image, 0, cuda_rv32i_ram_ptr(0), image.Length);
    cuda_rv32i_set_reg(0, 2, Sp);
    cuda_rv32i_set_entry(0, comp.entry);
    cuda_rv32i_set_block(1);
    cuda_rv32i_set_prefetch(0);
    cuda_rv32i_set_fpdispatch(fp);
    cuda_rv32i_step_all(300_000);                 // warm-up
    var sw = Stopwatch.StartNew();
    for (int i = 0; i < 3; i++) cuda_rv32i_step_all(300_000);
    sw.Stop();
    Console.WriteLine($"  {name,-7}   {3.0 * 300_000 / sw.Elapsed.TotalSeconds / 1e6,9:F2}");
    cuda_rv32i_set_fpdispatch(0);
    cuda_rv32i_shutdown();
}
Console.WriteLine();

// ── 3) Many-core throughput (packed + shared code, no prefetch) ──
// Gated behind --sweep and capped at 4096 cores (≤256 MB) so a stray run can
// never oversubscribe VRAM and hang the machine.
if (args.Contains("--sweep"))
{
    // Packing sweep: 32 lock-step cores/warp execute SIMD (one issue serves 32),
    // so block size is the throughput lever for same-image guests. block=1 = one
    // core/warp (no SIMD); block=256 = 8 warps/block. Max 32768 cores = 1 GB.
    Console.WriteLine("[packing] compute guest, 16384 cores, shared code, prefetch OFF");
    Console.WriteLine("  block   aggregate MIPS   per-core MIPS");
    Console.WriteLine("  ─────   ──────────────   ─────────────");
    foreach (int b in new[] { 1, 32, 64, 128, 256 })
    {
        double agg = Run(comp, 16384, b, true, false, 60_000, 0).mips;
        Console.WriteLine($"  {b,5}   {agg,14:F0}   {agg/16384,13:F3}");
    }

    Console.WriteLine("\n[fill] compute guest, block=256, shared code — scaling core count");
    Console.WriteLine("  cores    aggregate MIPS   per-core MIPS");
    Console.WriteLine("  ─────    ──────────────   ─────────────");
    foreach (int n in new[] { 1024, 4096, 16384, 32768 })
    {
        long budget = Math.Clamp(40_000_000L / n, 20_000L, 100_000L);
        double agg = Run(comp, n, 256, true, false, budget, 0).mips;
        Console.WriteLine($"  {n,5}    {agg,14:F0}   {agg/n,13:F3}");
    }
}
else Console.WriteLine("[throughput] skipped — pass --sweep to run (≤32768 cores, ≤1 GB).");

Console.WriteLine("\nMeasured: the instruction window does NOT speed up single core — a hot loop's");
Console.WriteLine("code is already L1-resident, so fetch isn't the bottleneck; the window just adds");
Console.WriteLine("overhead. Single-core is interpreter-bound (~2.5 MIPS); only a JIT breaks that.");
Console.WriteLine("Aggregate throughput scales with core count (bounded by VRAM / per-core RAM).");
return 0;

// Self-contained CUDA-C harness for the translated guest. /*BODY*/ is the
// switch(pc) emitted by RvJit; the rest is the register-resident execution
// model (R[32] locals → real GPU registers) + a RAM-only memory model.
partial class Program
{
    public const string JitTemplate = """
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

struct Hart { int halted; int exited; int exitcode; };
struct CoreMem { uint8_t* ram; uint32_t ram_size; };

template<class T> static __device__ __forceinline__ T ld_le(const uint8_t* __restrict__ p, uint32_t a){
    T v=0; for(uint32_t i=0;i<sizeof(T);i++) v |= (T)((uint32_t)p[a+i] << (8*i)); return v;
}
template<class T> static __device__ __forceinline__ void st_le(uint8_t* __restrict__ p, uint32_t a, T v){
    for(uint32_t i=0;i<sizeof(T);i++) p[a+i] = (uint8_t)((uint32_t)v >> (8*i));
}
template<class T> static __device__ __forceinline__ T mem_read(Hart& h, CoreMem& m, uint32_t a){
    if (a + sizeof(T) <= m.ram_size) return ld_le<T>(m.ram, a);
    return (T)0;
}
template<class T> static __device__ __forceinline__ void mem_write(Hart& h, CoreMem& m, uint32_t a, T v){
    if (a == 0x40000000u){ h.exitcode=(int)(uint32_t)v; h.exited=1; h.halted=1; return; }
    if (a + sizeof(T) <= m.ram_size) st_le<T>(m.ram, a, v);
}
// Inline RAM fast path: one bounds compare → direct RAM; else the slow path.
static __device__ __forceinline__ uint32_t jit_l8s (Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)(int8_t) (a<m.ram_size?m.ram[a]:mem_read<uint8_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l8u (Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)         (a<m.ram_size?m.ram[a]:mem_read<uint8_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l16s(Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)(int16_t)(a+2<=m.ram_size?ld_le<uint16_t>(m.ram,a):mem_read<uint16_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l16u(Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)         (a+2<=m.ram_size?ld_le<uint16_t>(m.ram,a):mem_read<uint16_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l32 (Hart&h,CoreMem&m,uint32_t a){ return                    a+4<=m.ram_size?ld_le<uint32_t>(m.ram,a):mem_read<uint32_t>(h,m,a); }
static __device__ __forceinline__ void jit_s8 (Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a<m.ram_size) m.ram[a]=(uint8_t)v; else mem_write<uint8_t>(h,m,a,(uint8_t)v); }
static __device__ __forceinline__ void jit_s16(Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a+2<=m.ram_size) st_le<uint16_t>(m.ram,a,(uint16_t)v); else mem_write<uint16_t>(h,m,a,(uint16_t)v); }
static __device__ __forceinline__ void jit_s32(Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a+4<=m.ram_size) st_le<uint32_t>(m.ram,a,v); else mem_write<uint32_t>(h,m,a,v); }
static __device__ __forceinline__ uint32_t jit_div (int32_t a,int32_t b){ return b==0?0xFFFFFFFFu:(a==(int32_t)0x80000000&&b==-1)?0x80000000u:(uint32_t)(a/b); }
static __device__ __forceinline__ uint32_t jit_divu(uint32_t a,uint32_t b){ return b==0?0xFFFFFFFFu:a/b; }
static __device__ __forceinline__ uint32_t jit_rem (int32_t a,int32_t b){ return b==0?(uint32_t)a:(a==(int32_t)0x80000000&&b==-1)?0u:(uint32_t)(a%b); }
static __device__ __forceinline__ uint32_t jit_remu(uint32_t a,uint32_t b){ return b==0?a:a%b; }

__global__ void jit_kernel(uint8_t* __restrict__ ram, uint32_t ram_size, uint32_t sp, uint32_t entry,
                           long long budget, uint32_t* __restrict__ regs_out, unsigned long long* __restrict__ cyc){
    uint32_t R[32];
    #pragma unroll
    for(int i=0;i<32;i++) R[i]=0;
    R[2]=sp;
    uint32_t pc=entry;
    long long bl=budget;
    Hart h; h.halted=0; h.exited=0; h.exitcode=0;
    CoreMem m; m.ram=ram; m.ram_size=ram_size;
    long long c0=clock64();
/*BODY*/
  out:
    {
        long long c1=clock64();
        #pragma unroll
        for(int i=0;i<32;i++) regs_out[i]=R[i];
        *cyc=(unsigned long long)(c1-c0);
    }
    return;
  interp:
    h.halted=1; goto out;   // unhandled opcode/target (jit_test guest never reaches here)
}

int main(int argc, char** argv){
    long long budget = argc>1 ? atoll(argv[1]) : 300000;
    uint32_t ram_size = /*RAMSIZE*/u;
    uint8_t* ram; cudaMallocManaged(&ram, ram_size);
    uint32_t* regs; cudaMallocManaged(&regs, 32*sizeof(uint32_t));
    unsigned long long* cyc; cudaMallocManaged(&cyc, sizeof(unsigned long long));
    for(uint32_t i=0;i<ram_size;i++) ram[i]=0;
    jit_kernel<<<1,1>>>(ram, ram_size, /*SP*/, /*ENTRY*/, 1000, regs, cyc);  // warmup
    cudaDeviceSynchronize();
    for(uint32_t i=0;i<ram_size;i++) ram[i]=0;
    cudaEvent_t e0,e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    jit_kernel<<<1,1>>>(ram, ram_size, /*SP*/, /*ENTRY*/, budget, regs, cyc);
    cudaEventRecord(e1);
    cudaError_t err=cudaDeviceSynchronize();
    if(err){ printf("CUDA err %d %s\n",(int)err,cudaGetErrorString(err)); return 1; }
    float ms=0; cudaEventElapsedTime(&ms,e0,e1);
    uint32_t S=/*SINK*/;
    uint32_t sink = ram[S] | (ram[S+1]<<8) | (ram[S+2]<<16) | (ram[S+3]<<24);
    printf("JITSINK=0x%08X JITMS=%.4f\n", sink, ms);
    return 0;
}
""";
}

// Minimal IMemoryBus over a byte[] so ElfLoader can build the image.
sealed class ArrayBus : IMemoryBus
{
    private readonly byte[] _r;
    public ArrayBus(byte[] r) => _r = r;
    public int RamSize => _r.Length;
    public System.Collections.Generic.IReadOnlyList<IPeripheral> Peripherals => System.Array.Empty<IPeripheral>();
    public byte ReadByte(uint a) => _r[a];
    public ushort ReadHalfWord(uint a) => (ushort)(_r[a] | (_r[a+1] << 8));
    public uint ReadWord(uint a) => (uint)(_r[a] | (_r[a+1]<<8) | (_r[a+2]<<16) | (_r[a+3]<<24));
    public void WriteByte(uint a, byte v) => _r[a] = v;
    public void WriteHalfWord(uint a, ushort v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); }
    public void WriteWord(uint a, uint v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); _r[a+2]=(byte)(v>>16); _r[a+3]=(byte)(v>>24); }
    public void Load(uint address, byte[] s, int o, int len) => System.Array.Copy(s, o, _r, (int)address, len);
}
