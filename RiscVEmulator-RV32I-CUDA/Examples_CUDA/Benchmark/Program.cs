using System.Diagnostics;
using RiscVEmulator.Core.Cuda;

// ── RV32IMA performance benchmark ────────────────────────────────────────────
// A frozen, self-contained, representative workload for measuring the CUDA core.
// It is a real branchy, indirection-heavy guest (data-driven init + render over a
// 16 MB working set) — i.e. the regime that runs at the honest ~2-3 MIPS, NOT a
// tight ALU microbench that flatters the number. Fixed work + a deterministic
// guest clock make the MIPS reproducible run-to-run, so it is a clean A/B for any
// future core/JIT change. Bounded to a few seconds.
//
//   Benchmark [measureSteps]   (default 12,000,000 ; ~one bounded launch series)

const uint DataBase = 0x00A00000, DataSizeAddr = 0x009FFFFC, StackPtr = 0x009FFF00;
const int  RamMB    = 16;
const int  Batch    = 500_000;     // per-launch budget (kept well under the WDDM TDR window)
const long Warm     = 1_000_000;   // untimed: ramp GPU clocks + first cold launch

long measure = 12_000_000;
if (args.Length > 0 && long.TryParse(args[0], out var m) && m > 0) measure = m;

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
ulong  hash = FbHash(emu.Framebuffer.PresentedPixels); // correctness / regression signature
Console.WriteLine($"benchmark: {steps / 1e6:F0}M steps in {sec:F2}s = {mips:F2} MIPS" +
                  $"   fbhash={hash:X16}  halted={emu.IsHalted}");
return 0;

static ulong FbHash(byte[] px)
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
