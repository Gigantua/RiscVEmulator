using System.Diagnostics;
using System.Globalization;
using RiscVEmulator.Core.Cuda;

// ── RV32IMA performance benchmark ────────────────────────────────────────────
// A frozen, self-contained, representative workload for measuring the CUDA core.
// It is a real branchy, indirection-heavy guest (data-driven init + render over a
// 16 MB working set) — i.e. the regime that runs at the honest ~2-3 MIPS, NOT a
// tight ALU microbench that flatters the number. Fixed work + a deterministic
// guest clock make the MIPS reproducible run-to-run, so it is a clean A/B for any
// future core/JIT change. Bounded to a few seconds.
//
//   Benchmark [measureSteps] [--csv]   (default 20,000,000 ; ~4.6 s, lands mid-window)
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

long measure = 20_000_000;
bool csv = false;
foreach (var a in args)
{
    if (a == "--csv") csv = true;
    else if (long.TryParse(a, out var m) && m > 0) measure = m;
}

string? elfPath = Find("benchmark.elf");
string? datPath = Find("benchmark.dat") ?? Find("doom1.wad");
if (elfPath == null) { Console.Error.WriteLine("benchmark.elf not found next to the executable."); return 1; }
if (datPath == null) { Console.Error.WriteLine("benchmark.dat not found next to the executable."); return 1; }

byte[] elf = File.ReadAllBytes(elfPath);
byte[] dat = File.ReadAllBytes(datPath);

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
