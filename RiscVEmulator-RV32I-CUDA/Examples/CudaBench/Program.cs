using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using RiscVEmulator.Core;

// Locale-independent output: '.' decimal separator regardless of host region.
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

// ── CUDA RV32I interpreter benchmark ──────────────────────────────────
// A single GPU thread interpreting RV32I is dependent-load-latency-bound
// (~2-3 MIPS/core). Aggregate throughput scales with core count until VRAM /
// per-core RAM limits. Default run: single-core latency for two guests.
// Pass --sweep for a (VRAM-capped) many-core throughput scaling sweep.

const string Lib = "rv32i_cuda";
[DllImport(Lib)] static extern int    cuda_rv32i_init(int n, uint memBytes);
[DllImport(Lib)] static extern int    cuda_rv32i_write_mem(int core, byte[] src, uint off, uint len);
[DllImport(Lib)] static extern int    cuda_rv32i_read_mem(int core, byte[] dst, uint off, uint len);
[DllImport(Lib)] static extern void   cuda_rv32i_set_reg(int core, int i, uint v);
[DllImport(Lib)] static extern void   cuda_rv32i_set_entry(int core, uint pc);
[DllImport(Lib)] static extern int    cuda_rv32i_step_all(int budget);
[DllImport(Lib)] static extern void   cuda_rv32i_shutdown();
// rvcud: optional RV32I→CUDA-uarch translator (separate kernel + host pretransform).
[DllImport(Lib)] static extern int    cuda_rvcud_set_code(byte[] src, uint len, uint baseAddr, uint entry);
[DllImport(Lib)] static extern int    cuda_rvcud_step_all(int budget);
[DllImport(Lib)] static extern ulong  cuda_rvcud_retired();

// rvcud (the RV32I→CUDA-uarch translator) is the DEFAULT execution path; pass --base to run the
// per-instruction kernel instead. (--rvcud still accepted for explicitness/back-compat.)
bool RV = !args.Contains("--base");

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
    string srcPath = Path.Combine(root, "Examples", "CudaBench", "Programs", name + ".c");
    string elfPath = Path.Combine(buildDir, name + ".elf");
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var a in new[] { "--target=riscv32-unknown-elf","-march=rv32i","-mabi=ilp32",
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

Console.WriteLine("Compiling guests (rv32i)...");
var comp  = Build("compute_guest");
var bench = Build("bench_guest");
var div   = Build("divergent_guest");
Console.WriteLine($"  compute  RO code: 0x{comp.codeLo:X5}..0x{comp.codeHi:X5}  ({comp.codeImg.Length} B)");
Console.WriteLine($"  data     RO code: 0x{bench.codeLo:X5}..0x{bench.codeHi:X5}  ({bench.codeImg.Length} B)");
Console.WriteLine($"  diverge  RO code: 0x{div.codeLo:X5}..0x{div.codeHi:X5}  ({div.codeImg.Length} B)\n");

// Per-core seed slot the divergent guest reads (below the 0x1000 code base).
const uint SeedAddr = 0x40;

// Run one config; returns (aggregate MIPS, verify-word = guest RAM[verifyAddr]).
(double mips, uint verify) Run(in (byte[] elf,uint entry,uint lo,uint hi,byte[] img) g,
                               int cores, int budget, uint verifyAddr, bool rvcud = false)
{
    var image = new byte[RamBytes];
    ElfLoader.Load(g.elf, new ArrayBus(image));
    if (cuda_rv32i_init(cores, RamBytes) != 0) return (-1, 0);
    for (int c = 0; c < cores; c++)
    {
        cuda_rv32i_write_mem(c, image, 0, (uint)image.Length);
        cuda_rv32i_write_mem(c, BitConverter.GetBytes((uint)c), SeedAddr, 4);  // per-core seed → divergence
        cuda_rv32i_set_reg(c, 2, Sp);
        cuda_rv32i_set_entry(c, g.entry);
    }
    if (rvcud) cuda_rvcud_set_code(image, (uint)image.Length, 0, g.entry);  // translate once; image word w = guest addr 4w (base 0)
    // (base kernel fetches live from per-core RAM written above — no separate code setup)
    Func<int,int> step = rvcud ? cuda_rvcud_step_all : cuda_rv32i_step_all;

    step(budget);                                // warm-up
    var sw = Stopwatch.StartNew();
    int iters = 3;
    for (int i = 0; i < iters; i++) step(budget);
    sw.Stop();
    uint verify = 0;
    if (verifyAddr != 0)
    {
        byte[] w = new byte[4];
        cuda_rv32i_read_mem(0, w, verifyAddr, 4);
        verify = BitConverter.ToUInt32(w);
    }
    cuda_rv32i_shutdown();
    return ((double)cores * budget * iters / sw.Elapsed.TotalSeconds / 1e6, verify);
}

// Single 1-core launch (no warm-up) for the correctness gate; returns the result-sink
// word and the retired guest-instruction count (rvcud reports it; rv32i retires == budget).
(uint verify, ulong retired) RunOnce(in (byte[] elf,uint entry,uint lo,uint hi,byte[] img) g,
                                     int budget, uint verifyAddr, bool rvcud)
{
    var image = new byte[RamBytes];
    ElfLoader.Load(g.elf, new ArrayBus(image));
    cuda_rv32i_init(1, RamBytes);
    cuda_rv32i_write_mem(0, image, 0, (uint)image.Length);
    cuda_rv32i_write_mem(0, BitConverter.GetBytes(0u), SeedAddr, 4);
    cuda_rv32i_set_reg(0, 2, Sp);
    cuda_rv32i_set_entry(0, g.entry);
    if (rvcud) { cuda_rvcud_set_code(image, (uint)image.Length, 0, g.entry); cuda_rvcud_step_all(budget); }
    else       cuda_rv32i_step_all(budget);   // base kernel fetches live from RAM
    ulong retired = rvcud ? cuda_rvcud_retired() : (ulong)budget;
    byte[] w = new byte[4]; cuda_rv32i_read_mem(0, w, verifyAddr, 4);
    uint verify = BitConverter.ToUInt32(w);
    cuda_rv32i_shutdown();
    return (verify, retired);
}

// Per-launch budgets are kept small (<<2 s of GPU work) so a never-halting
// guest can't trip the WDDM TDR watchdog and lock the machine.

// ── stable bench: base vs rvcud at a fixed core count, single process (GPU stays warm),
//    best-of-N so transient downclocks don't dominate. The grind metric.
if (args.Contains("--bench"))
{
    Console.WriteLine("[bench] 4096 cores — base vs rvcud aggregate MIPS (best of 3)");
    Console.WriteLine("  guest      base       rvcud      ratio");
    foreach (var (g, name) in new[] { (comp, "compute"), (bench, "data"), (div, "diverge") })
    {
        double b = 0, r = 0;
        for (int i = 0; i < 3; i++) { b = Math.Max(b, Run(g, 4096, 100_000, 0, false).mips);
                                      r = Math.Max(r, Run(g, 4096, 100_000, 0, true ).mips); }
        Console.WriteLine($"  {name,-7}  {b,9:F0}  {r,9:F0}   {r/b,6:F3}");
    }
    return 0;
}

// ── rvcud correctness gate ── a fused uop retires several guest instructions, so rvcud
// overshoots a guest-instruction budget by up to one uop's weight. To compare apples-to-
// apples we run rvcud for a budget, read its EXACT retired count R, then run rv32i for
// exactly R guest instructions and require the deterministic result-sink word to match.
{
    Console.WriteLine("[rvcud verify] rv32i vs rvcud, exact retired-count match (must match)");
    bool allpass = true;
    foreach (var (g, name, vad) in new[] { (comp, "compute", 0x3000u), (bench, "data", 0x4000u), (div, "diverge", 0x80u) })
    {
        var (bw, R)  = RunOnce(g, 200_000, vad, true);   // rvcud retires R guest instructions
        var (aw, _)  = RunOnce(g, (int)R, vad, false);   // rv32i runs exactly R
        bool ok = aw == bw; allpass &= ok;
        Console.WriteLine($"  {name,-7}  rv32i=0x{aw:X8}  rvcud=0x{bw:X8}  (R={R})  {(ok ? "PASS" : "FAIL")}");
    }
    Console.WriteLine(allpass ? "  → rvcud bit-identical ✓\n" : "  → rvcud MISMATCH ✗\n");
}

// ── 1) Single-core latency ──
// verify = result-sink word after a fixed budget; deterministic per guest, so it
// is a correctness fingerprint that must stay constant across kernel changes.
Console.WriteLine("[single core] 1 core — interpreter MIPS");
Console.WriteLine("  guest      MIPS/core    verify");
Console.WriteLine("  ───────    ─────────    ──────────");
foreach (var (g, name, va) in new[] { (comp, "compute", 0x3000u), (bench, "data", 0x4000u), (div, "diverge", 0x80u) })
{
    var (mips, verify) = Run(g, 1, 300_000, va, RV);
    Console.WriteLine($"  {name,-7}    {mips,9:F2}    0x{verify:X8}");
}
Console.WriteLine();

// ── 1b) Single-warp: 32 cores packed into one 32-thread block (= 1 warp) ──
// block=32, grid=1, so all 32 lanes issue in SIMT lockstep within a single warp.
// compute/data run lockstep (≈32× the single-core rate, free SIMT parallelism);
// diverge gives each lane its own seed, exposing the warp-divergence penalty.
Console.WriteLine("[single warp] 32 cores (1 block = 1 warp) — interpreter MIPS");
Console.WriteLine("  guest      warp MIPS    MIPS/core");
Console.WriteLine("  ───────    ─────────    ─────────");
foreach (var (g, name) in new[] { (comp, "compute"), (bench, "data"), (div, "diverge") })
{
    double agg = Run(g, 32, 100_000, 0, RV).mips;
    Console.WriteLine($"  {name,-7}    {agg,9:F2}    {agg / 32,9:F2}");
}
Console.WriteLine();

// ── 2) Many-core throughput (packed + shared code) ──
// Gated behind --sweep and capped at 32768 cores (≤1 GB) so a stray run can
// never oversubscribe VRAM and hang the machine.
if (args.Contains("--sweep"))
{
    Console.WriteLine("[fill] scaling core count — uniform (compute, lockstep) vs divergent (per-core seed)");
    Console.WriteLine("  cores    compute aggMIPS  per-core    diverge aggMIPS  per-core");
    Console.WriteLine("  ─────    ──────────────  ────────    ──────────────  ────────");
    foreach (int n in new[] { 32, 256, 1024, 4096, 16384, 32768 })
    {
        int budget = Math.Clamp(40_000_000 / n, 20_000, 100_000);
        double cagg = Run(comp, n, budget, 0, RV).mips;
        double dagg = Run(div,  n, budget, 0, RV).mips;
        Console.WriteLine($"  {n,5}    {cagg,14:F0}  {cagg/n,8:F3}    {dagg,14:F0}  {dagg/n,8:F3}");
    }
}
else Console.WriteLine("[throughput] skipped — pass --sweep to run (≤32768 cores, ≤1 GB).");

Console.WriteLine("\nSingle-core is interpreter-bound (~2.5 MIPS); aggregate throughput scales with");
Console.WriteLine("core count (bounded by VRAM / per-core RAM).");
return 0;

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
