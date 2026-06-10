using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using RiscVEmulator.Core;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

// ── CudaMath ──────────────────────────────────────────────────────────────
// clang-compiled rv32i math benchmarks, each run TO COMPLETION (the guest halts via
// ebreak) on the CUDA emulator and verified against the SAME algorithm computed
// independently in C#. Because the guest halts, the retired-instruction count is the
// exact instruction count of the whole program, and the result word is a function of
// the complete execution — a wrong instruction anywhere changes it.
//   MIPS = exact retired rv32i instructions / wall time (includes all launch overhead).
// Default: rvcud + exec_block JIT, 1 core and 32 cores (one warp, lockstep).
// --base additionally runs the per-instruction rv32i reference kernel (slow).

const string Lib = "rv32i_cuda";
[DllImport(Lib)] static extern int   cuda_rv32i_init(int n, uint memBytes);
[DllImport(Lib)] static extern int   cuda_rv32i_write_mem(int core, byte[] src, uint off, uint len);
[DllImport(Lib)] static extern int   cuda_rv32i_read_mem(int core, byte[] dst, uint off, uint len);
[DllImport(Lib)] static extern void  cuda_rv32i_set_reg(int core, int i, uint v);
[DllImport(Lib)] static extern void  cuda_rv32i_set_entry(int core, uint pc);
[DllImport(Lib)] static extern int   cuda_rv32i_step_all(int budget);
[DllImport(Lib)] static extern void  cuda_rv32i_shutdown();
[DllImport(Lib)] static extern int   cuda_rvcud_set_code(byte[] src, uint len, uint baseAddr, uint entry);
[DllImport(Lib)] static extern int   cuda_rvcud_step_all(int budget);
[DllImport(Lib)] static extern ulong cuda_rvcud_retired();

bool runBase = args.Contains("--base");

const int  RamBytes = 32 * 1024;
const uint Sp       = 0x00007F00;
const uint Sink     = 0x7000;
string clang  = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln"))) root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("RiscVEmulator.sln not found"); return 2; }
string buildDir = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);

(byte[] elf, uint entry) Build(string name)
{
    string srcPath = Path.Combine(root, "Examples", "CudaMath", "Programs", name + ".c");
    string elfPath = Path.Combine(buildDir, name + ".elf");
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var a in new[] { "--target=riscv32-unknown-elf","-march=rv32i","-mabi=ilp32",
            "-nostdlib","-nostartfiles","-O3","-fno-builtin","-ffreestanding",
            "-fuse-ld=lld","-Wl,-e,_start","-Wl,--image-base=0x1000", srcPath, "-o", elfPath })
        psi.ArgumentList.Add(a);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    if (p.ExitCode != 0) { Console.Error.WriteLine(err); Environment.Exit(2); }
    byte[] elfData = File.ReadAllBytes(elfPath);
    var image = new byte[RamBytes];
    uint e = ElfLoader.Load(elfData, new ArrayBus(image));
    return (elfData, e);
}

// Run one guest to halt. Returns per-core sink words, total core-0 retired instrs, wall seconds.
(uint[] sinks, ulong instrs, double secs) Go((byte[] elf, uint entry) g, int cores, bool rvcud)
{
    var image = new byte[RamBytes];
    ElfLoader.Load(g.elf, new ArrayBus(image));
    if (cuda_rv32i_init(cores, RamBytes) != 0) throw new InvalidOperationException("init failed");
    for (int c = 0; c < cores; c++)
    {
        cuda_rv32i_write_mem(c, image, 0, (uint)image.Length);
        cuda_rv32i_set_reg(c, 2, Sp);
        cuda_rv32i_set_entry(c, g.entry);
    }
    if (rvcud) cuda_rvcud_set_code(image, (uint)image.Length, 0, g.entry);
    Func<int,int> st = rvcud ? cuda_rvcud_step_all : cuda_rv32i_step_all;
    int budget = rvcud ? 50_000_000 : 5_000_000;     // per-launch caps: well under the WDDM TDR watchdog
    ulong total = 0;
    var sw = Stopwatch.StartNew();
    for (int i = 0; i < 1000; i++)
    {
        st(budget);
        if (rvcud)
        {
            ulong r = cuda_rvcud_retired();              // exact per-call retired count (core 0)
            total += r;
            if (r == 0) break;                           // halted
        }
        else
        {
            // base kernel has no retired counter; the sink word is only written right before the
            // halting ebreak, so a nonzero sink is the halt probe (base MIPS uses rvcud's count).
            var probe = new byte[4]; cuda_rv32i_read_mem(0, probe, Sink, 4);
            if (BitConverter.ToUInt32(probe) != 0) break;
        }
    }
    sw.Stop();
    var sinks = new uint[cores]; var w = new byte[4];
    for (int c = 0; c < cores; c++) { cuda_rv32i_read_mem(c, w, Sink, 4); sinks[c] = BitConverter.ToUInt32(w); }
    cuda_rv32i_shutdown();
    return (sinks, total, sw.Elapsed.TotalSeconds);
}

// ── C# mirrors: the SAME algorithms, transcribed exactly (uint wraparound semantics) ──
static uint MirrorSieve()
{
    var bits = new uint[2048];
    uint acc = 0;
    for (uint rep = 0; rep < 20u; rep++)
    {
        Array.Clear(bits);
        for (uint p = 2; p < 256u; p++)
        {
            if ((bits[p >> 5] & (1u << (int)(p & 31u))) != 0) continue;
            for (uint m = p * p; m < 65536u; m += p) bits[m >> 5] |= 1u << (int)(m & 31u);
        }
        uint cnt = 0;
        for (uint n = 2; n < 65536u; n++)
            if ((bits[n >> 5] & (1u << (int)(n & 31u))) == 0) cnt++;
        acc = acc * 31u + cnt + rep;
    }
    return acc;
}
static uint MirrorFib()
{
    uint a = 0, b = 1;
    for (uint i = 0; i < 50000000u; i++) { uint t = a + b; a = b; b = t; }
    return a;
}
static uint FixmulMag(uint a, uint b)
{
    uint lh = 0, ll = 0, ah = 0, al = a;
    while (b != 0)
    {
        if ((b & 1u) != 0) { uint t = ll + al; lh += ah + (t < ll ? 1u : 0u); ll = t; }
        ah = (ah << 1) | (al >> 31); al <<= 1;
        b >>= 1;
    }
    return (ll >> 16) | (lh << 16);
}
static int Fmul(int a, int b)
{
    uint ua = a < 0 ? (uint)(-a) : (uint)a;
    uint ub = b < 0 ? (uint)(-b) : (uint)b;
    uint m = FixmulMag(ua, ub);
    return ((a < 0) != (b < 0)) ? -(int)m : (int)m;
}
static uint MirrorMandel()
{
    int x0 = -(2 << 16) - (1 << 15);
    int y0 = -(1 << 16);
    int dx = (3 << 16) / 96 + ((1 << 15) / 96);
    int dy = (2 << 16) / 64;
    uint h = 0x811C9DC5u;
    for (int py = 0; py < 64; py++)
    {
        int ci = y0 + dy * py;
        for (int px = 0; px < 96; px++)
        {
            int cr = x0 + dx * px;
            int zr = 0, zi = 0;
            uint it = 0;
            while (it < 64u)
            {
                int zr2 = Fmul(zr, zr), zi2 = Fmul(zi, zi);
                if (zr2 + zi2 > (4 << 16)) break;
                int nzr = zr2 - zi2 + cr;
                zi = 2 * Fmul(zr, zi) + ci;
                zr = nzr;
                it++;
            }
            h = (h ^ it) * 16777619u;
        }
    }
    return h;
}
static uint MirrorCrc32()
{
    var buf = new byte[8192];
    uint x = 0xC0FFEE01u;
    for (uint i = 0; i < 8192u; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (byte)(x >> 24); }
    uint acc = 0;
    for (uint rep = 0; rep < 64u; rep++)
    {
        uint crc = 0xFFFFFFFFu ^ rep;
        for (uint i = 0; i < 8192u; i++)
        {
            crc ^= buf[i];
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        acc = acc * 33u + ~crc;
    }
    return acc;
}
static uint MirrorCollatz()
{
    uint total = 0;
    for (uint n = 1; n <= 100000u; n++)
    {
        uint v = n;
        while (v != 1u) { v = (v & 1u) != 0 ? 3u * v + 1u : v >> 1; total++; }
    }
    return total;
}
static uint MirrorMatmul()
{
    const int N = 24;
    var A = new uint[N*N]; var B = new uint[N*N]; var C = new uint[N*N];
    uint x = 0x12345678u;
    for (int i = 0; i < N*N; i++)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; A[i] = x >> 16;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; B[i] = x >> 16;
    }
    for (int rep = 0; rep < 32; rep++)
    {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
            {
                uint s = 0;
                for (int k = 0; k < N; k++) s += A[i*N+k] * B[k*N+j];
                C[i*N+j] = s;
            }
        for (int i = 0; i < N*N; i++) A[i] = (C[i] >> 7) | 1u;
    }
    uint h = 0x811C9DC5u;
    for (int i = 0; i < N*N; i++) h = (h ^ C[i]) * 16777619u;
    return h;
}

var tests = new (string name, Func<uint> mirror)[]
{
    ("fib",     MirrorFib),       // pure dependent ALU chain
    ("sieve",   MirrorSieve),     // memory + branches
    ("crc32",   MirrorCrc32),     // bit ops + byte loads
    ("collatz", MirrorCollatz),   // data-dependent branches
    ("mandel",  MirrorMandel),    // fixed-point soft-multiply
    ("matmul",  MirrorMatmul),    // soft-multiply + array walks
};

Console.WriteLine("Compiling guests (clang -O3 --target=riscv32 -march=rv32i)...");
var built = tests.Select(t => (t.name, t.mirror, g: Build(t.name + "_guest"))).ToArray();

Console.WriteLine("computing C# reference results...");
var expected = built.Select(t => t.mirror()).ToArray();

// GPU clock warm-up (untimed)
Go(built[0].g, 1, true);

Console.WriteLine();
Console.WriteLine("[cudamath] run-to-halt, verified vs C# mirror; MIPS = exact retired instrs / wall");
Console.WriteLine($"  guest      result       instrs(M)   1-core MIPS   warp32 aggMIPS{(runBase ? "   base MIPS  speedup" : "")}   verdict");
Console.WriteLine($"  ───────    ──────────   ─────────   ───────────   ──────────────{(runBase ? "   ─────────  ───────" : "")}   ───────");
bool allok = true;
for (int t = 0; t < built.Length; t++)
{
    var (s1, instrs, sec1) = Go(built[t].g, 1, true);
    var (sw32, _, sec32)   = Go(built[t].g, 32, true);
    bool ok = s1[0] == expected[t] && sw32.All(v => v == expected[t]);
    double mips1 = instrs / sec1 / 1e6;
    double aggMips = 32.0 * instrs / sec32 / 1e6;     // lockstep: every core retires the same count
    string extra = "";
    if (runBase)
    {
        var (sb, _, secb) = Go(built[t].g, 1, false);
        ok &= sb[0] == expected[t];
        double bm = instrs / secb / 1e6;
        extra = $"   {bm,9:F1}  {mips1/bm,6:F1}×";
    }
    allok &= ok;
    Console.WriteLine($"  {built[t].name,-7}    0x{s1[0]:X8}   {instrs/1e6,9:F1}   {mips1,11:F1}   {aggMips,14:F0}{extra}   {(ok ? "PASS" : $"FAIL (expect 0x{expected[t]:X8})")}");
}
Console.WriteLine(allok ? "\n  → all results bit-exact vs the C# mirrors ✓" : "\n  → MISMATCH ✗");
return allok ? 0 : 1;

sealed class ArrayBus : IMemoryBus
{
    private readonly byte[] _r;
    public ArrayBus(byte[] r) => _r = r;
    public int RamSize => _r.Length;
    public IReadOnlyList<IPeripheral> Peripherals => Array.Empty<IPeripheral>();
    public byte ReadByte(uint a) => _r[a];
    public ushort ReadHalfWord(uint a) => (ushort)(_r[a] | (_r[a+1] << 8));
    public uint ReadWord(uint a) => (uint)(_r[a] | (_r[a+1]<<8) | (_r[a+2]<<16) | (_r[a+3]<<24));
    public void WriteByte(uint a, byte v) => _r[a] = v;
    public void WriteHalfWord(uint a, ushort v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); }
    public void WriteWord(uint a, uint v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); _r[a+2]=(byte)(v>>16); _r[a+3]=(byte)(v>>24); }
    public void Load(uint address, byte[] s, int o, int len) => Array.Copy(s, o, _r, (int)address, len);
}
