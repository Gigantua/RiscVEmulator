using System.Diagnostics;
using System.Globalization;
using RiscVEmulator.Core.Cuda;

// ── RV32I performance benchmark ────────────────────────────────────────────
// A frozen, self-contained, representative workload for measuring the CUDA core.
// It is a real branchy, indirection-heavy guest (data-driven init + render over a
// 16 MB working set) — i.e. the regime that runs at the honest ~2-3 MIPS, NOT a
// tight ALU microbench that flatters the number. Fixed work + a deterministic
// guest clock make the MIPS reproducible run-to-run, so it is a clean A/B for any
// future core/JIT change. Bounded to a few seconds.
//
//   Benchmark [measureSteps] [--csv]   (default 20,000,000 ; ~4.6 s, lands mid-window)
//   Benchmark --cores N                N independent guests headless; AGGREGATE MIPS
//     (throughput: read-only image shared across cores).
//
// This benchmark is the REFEREE for all CUDA-core perf work. Two signatures gate
// correctness on every A/B run:
//   • statehash — FNV-1a over guest RAM [0, 0xA00000) (code+heap+stack, excludes
//     the constant WAD region). This reflects actual guest computation and is the
//     real bit-exact regression check.
//   • fbhash    — FNV-1a over the framebuffer. The FB is BLACK during cold-init,
//     so this only confirms "still black" — a weak signal kept for continuity.

// Locale-independent output: '.' decimal separator everywhere, so the --csv
// line is machine-parseable (no ',' decimal colliding with the field comma) and
// numbers are reproducible regardless of the host's regional settings.
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

const uint DataBase = 0x00A00000, DataSizeAddr = 0x009FFFFC, StackPtr = 0x009FFF00;
const uint StateHashLen = 0x00A00000;  // hash RAM [0, WAD base): code+heap+stack only
const int  RamMB    = 16;
const int  Batch    = 500_000;     // per-launch budget (kept well under the WDDM TDR window)
const long Warm     = 1_000_000;   // untimed: ramp GPU clocks + first cold launch

bool csv  = args.Any(a => a == "--csv");
int  cores = 1;
{
    int ci = Array.IndexOf(args, "--cores");
    if (ci >= 0 && ci + 1 < args.Length && int.TryParse(args[ci + 1], out var c) && c > 0) cores = c;
}
// positional (non-flag, and not the value following --cores) args → step count
string[] pos = args.Where((a, i) => !a.StartsWith("--")
                          && !(i > 0 && args[i - 1] == "--cores")).ToArray();

long measure = 20_000_000;
if (pos.Length > 0 && long.TryParse(pos[0], out var m) && m > 0) measure = m;

string? elfPath = Find("benchmark.elf");
string? datPath = Find("benchmark.dat") ?? Find("doom1.wad");
if (elfPath == null) { Console.Error.WriteLine("benchmark.elf not found next to the executable."); return 1; }
if (datPath == null) { Console.Error.WriteLine("benchmark.dat not found next to the executable."); return 1; }

byte[] elf = File.ReadAllBytes(elfPath);
byte[] dat = File.ReadAllBytes(datPath);

if (cores > 1) return RunThroughput(cores, elf, dat);

using var emu = new CudaEmulator(RamMB * 1024 * 1024);
emu.OutputHandler   = _ => { };
emu.DeterministicTime = true;                          // reproducible instruction stream
uint entry = emu.LoadElf(elf);
emu.LoadBytes(DataSizeAddr, BitConverter.GetBytes((uint)dat.Length));
emu.LoadBytes(DataBase, dat);
emu.CommitImage();
emu.SetReg(2, StackPtr);
emu.SetEntry(entry);

emu.StepN((int)Warm);                                   // warm-up (not timed)

var sw = Stopwatch.StartNew();
long steps = 0;
while (steps < measure && !emu.IsHalted) { emu.StepN(Batch); steps += Batch; }
sw.Stop();

double sec  = sw.Elapsed.TotalSeconds;
double mips = steps / sec / 1e6;

// fbhash: framebuffer signature (weak — FB is black during cold-init).
ulong fbHash = FnvFb(emu.Framebuffer.PresentedPixels);

// statehash: bit-exact RAM state signature [0, 0xA00000) — the real regression
// check, reflecting actual guest computation (excludes the constant WAD region).
var ram = new byte[StateHashLen];
emu.ReadRam(ram, 0, StateHashLen);
ulong stateHash = Fnv(ram);

if (csv)
{
    // steps,seconds,mips,fbhash,statehash — single machine-parseable line.
    Console.WriteLine($"{steps},{sec:F4},{mips:F4},{fbHash:X16},{stateHash:X16}");
}
else
{
    Console.WriteLine($"benchmark: {steps / 1e6:F0}M steps in {sec:F2}s = {mips:F2} MIPS" +
                      $"   fbhash={fbHash:X16}  statehash={stateHash:X16}  halted={emu.IsHalted}");
}
return 0;

// FNV-1a over an entire byte buffer.
static ulong Fnv(byte[] data)
{
    ulong h = 1469598103934665603UL;
    foreach (byte b in data)
        h = (h ^ b) * 1099511628211UL;
    return h;
}

// ── Throughput mode: N independent guests, one GPU thread each ────────────────
// A single GPU thread is latency-bound (~4.27 MIPS — it can't ILP out of the
// per-step fetch/load scoreboard stalls). The GPU's real win is THROUGHPUT: many
// independent guests run concurrently and share the per-warp memory stall, so
// aggregate MIPS scales far past one lane. Each guest gets the SAME frozen image,
// but the read-only .text+rodata span is deduplicated into ONE shared, cache-
// resident device buffer (CommitImageToAllCores installs it for N > 1) — N cores
// then share a single instruction-fetch working set instead of N copies.
// Bounded: one warm-up launch + a fixed series of ≤250k-step launches (TDR-safe).
int RunThroughput(int n, byte[] elfData, byte[] datData)
{
    // VRAM guard (mirrors Examples/CudaDoom): each instance needs ~16 MB of
    // per-core RAM. Oversubscribing a display GPU's VRAM thrashes migration and
    // can hard-lock the machine, so refuse anything over ~1 GB of guest RAM.
    long totalRamMB = (long)n * RamMB;
    if (totalRamMB > 1024)
    {
        Console.Error.WriteLine($"Refusing {n} instances = {totalRamMB} MB guest RAM (> 1 GB). " +
            $"That risks oversubscribing VRAM on a display GPU and hanging the machine. " +
            $"Use --cores {1024 / RamMB} or fewer.");
        return 1;
    }

    Console.WriteLine($"Throughput: {n} independent guests (one GPU thread each), shared read-only image...");
    using var emu = new CudaEmulator(RamMB * 1024 * 1024, nCores: n);
    emu.OutputHandler     = _ => { };
    emu.DeterministicTime = true;
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(DataSizeAddr, BitConverter.GetBytes((uint)datData.Length));
    emu.LoadBytes(DataBase, datData);
    emu.CommitImageToAllCores(StackPtr, entry);

    // Per-launch budget: keep each kernel well under the WDDM TDR window. Scale
    // down with core count (more cores = more work per launch) but clamp to the
    // TDR-safe band. One warm-up launch (GPU clock ramp + guest init), then a
    // short fixed series — never an unbounded loop or a core-count sweep.
    long budget = Math.Clamp(8_000_000L / n, 50_000L, 250_000L);
    const int iters = 8;

    emu.StepN((int)budget);                         // warm-up (not timed)
    var sw = Stopwatch.StartNew();
    for (int i = 0; i < iters && !emu.IsHalted; i++) emu.StepN((int)budget);
    sw.Stop();

    double sec       = sw.Elapsed.TotalSeconds;
    long   perCore   = budget * iters;
    double aggMips   = (double)n * perCore / sec / 1e6;
    double coreMips  = aggMips / n;
    double roSavedMB = (double)emu.SharedRoBytes * (n - 1) / (1024.0 * 1024.0);
    Console.WriteLine($"throughput: {n} guests x {perCore / 1e6:F1}M steps in {sec:F2}s");
    Console.WriteLine($"  AGGREGATE = {aggMips:F2} MIPS   ({coreMips:F2} MIPS/core, vs ~4.27 single)");
    Console.WriteLine($"  shared RO image = {emu.SharedRoBytes / 1024.0:F0} KB (one copy) " +
                      $"=> ~{roSavedMB:F1} MB of duplicate RO working set deduplicated across {n} cores");
    return 0;
}

// FNV-1a over the RGB channels of an RGBA framebuffer (skips alpha).
static ulong FnvFb(byte[] px)
{
    ulong h = 1469598103934665603UL;
    for (int i = 0; i + 3 < px.Length; i += 4)
    {
        h = (h ^ px[i])     * 1099511628211UL;
        h = (h ^ px[i + 1]) * 1099511628211UL;
        h = (h ^ px[i + 2]) * 1099511628211UL;
    }
    return h;
}

static string? Find(string name)
{
    string baseDir = AppContext.BaseDirectory;
    string p = Path.Combine(baseDir, name);
    if (File.Exists(p)) return p;
    return null;
}

