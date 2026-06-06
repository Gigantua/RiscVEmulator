using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core;

// ── CUDA RV32I interpreter benchmark ──────────────────────────────────
// A single GPU thread interpreting RV32I is dependent-load-latency-bound
// (~2-3 MIPS/core). Aggregate throughput scales with core count until VRAM /
// per-core RAM limits. Default run: single-core latency for two guests.
// Pass --sweep for a (VRAM-capped) many-core throughput scaling sweep.

const string Lib = "rv32i_cuda";
[DllImport(Lib)] static extern int    cuda_rv32i_init(int n, uint ram, uint fbW, uint fbH, uint pcm);
[DllImport(Lib)] static extern int    cuda_rv32i_load_ram(int core, byte[] src, uint off, uint len);
[DllImport(Lib)] static extern int    cuda_rv32i_read_ram(int core, byte[] dst, uint off, uint len);
[DllImport(Lib)] static extern void   cuda_rv32i_set_reg(int core, int i, uint v);
[DllImport(Lib)] static extern void   cuda_rv32i_set_entry(int core, uint pc);
[DllImport(Lib)] static extern int    cuda_rv32i_step_all(int budget);
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

// Run one config; returns (aggregate MIPS, verify-word = guest RAM[verifyAddr]).
(double mips, uint verify) Run(in (byte[] elf,uint entry,uint lo,uint hi,byte[] img) g,
                               int cores, int budget, uint verifyAddr)
{
    var image = new byte[RamBytes];
    ElfLoader.Load(g.elf, new ArrayBus(image));
    if (cuda_rv32i_init(cores, RamBytes, 64, 64, 4096) != 0) return (-1, 0);
    for (int c = 0; c < cores; c++)
    {
        cuda_rv32i_load_ram(c, image, 0, (uint)image.Length);
        cuda_rv32i_set_reg(c, 2, Sp);
        cuda_rv32i_set_entry(c, g.entry);
    }

    cuda_rv32i_step_all(budget);                 // warm-up
    var sw = Stopwatch.StartNew();
    int iters = 3;
    for (int i = 0; i < iters; i++) cuda_rv32i_step_all(budget);
    sw.Stop();
    uint verify = 0;
    if (verifyAddr != 0)
    {
        byte[] w = new byte[4];
        cuda_rv32i_read_ram(0, w, verifyAddr, 4);
        verify = BitConverter.ToUInt32(w);
    }
    cuda_rv32i_shutdown();
    return ((double)cores * budget * iters / sw.Elapsed.TotalSeconds / 1e6, verify);
}

// Per-launch budgets are kept small (<<2 s of GPU work) so a never-halting
// guest can't trip the WDDM TDR watchdog and lock the machine.

// ── 1) Single-core latency ──
Console.WriteLine("[single core] 1 core — interpreter MIPS");
Console.WriteLine("  guest      MIPS/core");
Console.WriteLine("  ───────    ─────────");
foreach (var (g, name) in new[] { (comp, "compute"), (bench, "data") })
    Console.WriteLine($"  {name,-7}    {Run(g, 1, 300_000, 0).mips,9:F2}");
Console.WriteLine();

// ── 2) Many-core throughput (packed + shared code) ──
// Gated behind --sweep and capped at 32768 cores (≤1 GB) so a stray run can
// never oversubscribe VRAM and hang the machine.
if (args.Contains("--sweep"))
{
    Console.WriteLine("[fill] compute guest, block=256, shared code — scaling core count");
    Console.WriteLine("  cores    aggregate MIPS   per-core MIPS");
    Console.WriteLine("  ─────    ──────────────   ─────────────");
    foreach (int n in new[] { 256, 1024, 4096 })
    {
        int budget = Math.Clamp(40_000_000 / n, 20_000, 100_000);
        double agg = Run(comp, n, budget, 0).mips;
        Console.WriteLine($"  {n,5}    {agg,14:F0}   {agg/n,13:F3}");
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
