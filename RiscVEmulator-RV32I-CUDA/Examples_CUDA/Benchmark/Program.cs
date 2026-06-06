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
//   Benchmark --prof [profSteps] (default 12,000,000): profile THIS (cold-init)
//     regime — dynamic opcode mix, taken-jump-length summary, and a hot-PC
//     coverage curve (how many distinct PCs cover 50/80/90/95/99% of executed
//     instructions). Read-only; does not change the normal-run numbers.

const uint DataBase = 0x00A00000, DataSizeAddr = 0x009FFFFC, StackPtr = 0x009FFF00;
const int  RamMB    = 16;
const int  Batch    = 500_000;     // per-launch budget (kept well under the WDDM TDR window)
const long Warm     = 1_000_000;   // untimed: ramp GPU clocks + first cold launch

bool prof = args.Any(a => a == "--prof");
string[] pos = args.Where(a => !a.StartsWith("--")).ToArray();   // positional (non-flag) args

long measure = 12_000_000;
if (!prof && pos.Length > 0 && long.TryParse(pos[0], out var m) && m > 0) measure = m;

// Bounded profile budget (single-thread, ~3s at ~4 MIPS). Override with the
// positional arg after --prof.
int profSteps = 12_000_000;
if (prof && pos.Length > 0 && int.TryParse(pos[0], out var ps) && ps > 0) profSteps = ps;

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

if (prof)
{
    // Re-establish the post-warm state: re-load the (possibly profiler-dirtied)
    // RAM image, reset sp/entry/halt, and re-warm. Each profiling pass calls this
    // first, so the opcode profiler and the PC-hist profiler both characterize the
    // IDENTICAL instruction window [entry, entry+warm+budget) — neither pass is
    // perturbed by the other (and a pass that runs to guest halt can't starve the
    // next one). The render kernel mutates guest RAM, so a plain re-launch without
    // this would profile a different, drifted window.
    void Reset()
    {
        emu.SetHalted(false);
        emu.CommitImage();
        emu.SetReg(2, StackPtr);
        emu.SetEntry(entry);
        emu.StepN((int)Warm);
    }
    RunProfile(emu, profSteps, Reset);
    return 0;
}

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

// ── --prof: profile the cold-init regime (opcode mix, jumps, hot-PC coverage) ─
// `reset` re-establishes the post-warm guest state; it is called before each
// profiling pass so all passes measure the same instruction window.
static void RunProfile(CudaEmulator emu, int budget, Action reset)
{
    // (instr>>2)&0x1F bucket → RV32 opcode name (matches the device's profiler idx).
    // Only the buckets this guest actually uses are named; the rest stay "op<idx>".
    string[] names = new string[32];
    for (int i = 0; i < 32; i++) names[i] = $"op{i:00}";
    names[0]  = "LOAD";   names[3]  = "FENCE";  names[4]  = "OP-IMM"; names[5]  = "AUIPC";
    names[8]  = "STORE";  names[11] = "AMO";    names[12] = "OP";     names[13] = "LUI";
    names[24] = "BRANCH"; names[25] = "JALR";   names[27] = "JAL";    names[28] = "SYSTEM";

    Console.WriteLine($"=== Benchmark --prof (cold-init regime, budget={budget:N0} steps) ===");

    // (a) Dynamic opcode mix. prof[prev*32+cur] is the adjacent-pair matrix; the
    //     per-opcode dynamic count is its column sum (incoming) — equivalently the
    //     number of times opcode `cur` was the executed instruction. prof[1027] is
    //     the device's total step count (authoritative denominator).
    emu.ProfReset();
    int prc = emu.Profile(budget);
    if (prc != 0) { Console.Error.WriteLine($"profile failed (CUDA error {prc})"); return; }
    ulong[] prof = emu.ProfRead();
    var op = new ulong[32];
    ulong opTotal = 0;
    for (int cur = 0; cur < 32; cur++)
    {
        ulong c = 0;
        for (int prev = 0; prev < 32; prev++) c += prof[prev * 32 + cur];
        op[cur] = c; opTotal += c;
    }
    ulong total = prof[1027];   // device step count (includes the very first instr, which has no predecessor)

    Console.WriteLine();
    Console.WriteLine($"-- dynamic opcode mix ({opTotal:N0} classified of {total:N0} steps) --");
    var order = Enumerable.Range(0, 32).Where(i => op[i] > 0).OrderByDescending(i => op[i]).ToArray();
    foreach (int i in order)
        Console.WriteLine($"  {names[i],-8} {op[i],14:N0}  {(opTotal > 0 ? 100.0 * op[i] / opTotal : 0),6:F2}%");
    if (order.Length >= 1)
    {
        var top5 = order.Take(5).Select(i => $"{names[i]} {(opTotal > 0 ? 100.0 * op[i] / opTotal : 0):F1}%");
        Console.WriteLine($"  top-5: {string.Join(", ", top5)}");
    }

    // (b) Taken-jump-length summary (per control op). The device bins forward/back
    //     magnitudes; report count and average |distance| in bytes per op class.
    ulong[] jump = emu.JumpRead();
    string[] jn = { "BRANCH(taken)", "JAL", "JALR" };
    Console.WriteLine();
    Console.WriteLine("-- taken control-flow jumps (sequential not-taken flow excluded) --");
    for (int o = 0; o < 3; o++)
    {
        ulong cnt = jump[o * 64 + 50], sum = jump[o * 64 + 51];
        ulong fwd = 0, bwd = 0;
        for (int b = 0; b < 25; b++) fwd += jump[o * 64 + b];
        for (int b = 0; b < 25; b++) bwd += jump[o * 64 + 25 + b];
        double avg = cnt > 0 ? (double)sum / cnt : 0;
        Console.WriteLine($"  {jn[o],-14} count={cnt,12:N0}  fwd={fwd,12:N0}  back={bwd,12:N0}  avg|dist|={avg,8:F0} B");
    }

    // (c) Hot-PC coverage curve. Bin per-PC execution counts over the guest .text
    //     span, sort descending, and report how many distinct PCs cover each
    //     percentile of executed instructions — the key feasibility signal for a
    //     hot-region JIT (a small N means a tiny JIT footprint captures most work).
    //     The opcode pass (a)+(b) already consumed `budget` steps (and may have run
    //     the guest to halt), so reset to the post-warm state first: this pass then
    //     covers the identical instruction window the opcode mix was measured over.
    (uint lo, uint hi) = emu.CodeSpan;
    if (hi <= lo) { Console.WriteLine("\n-- hot-PC coverage: no RO code span in ELF --"); return; }
    uint words = (hi - lo) / 4u;
    reset();
    ulong[] pch = emu.PcHist(budget, lo, words);

    ulong execTotal = 0; int distinct = 0;
    for (int i = 0; i < pch.Length; i++) { execTotal += pch[i]; if (pch[i] > 0) distinct++; }

    Console.WriteLine();
    Console.WriteLine($"-- hot-PC coverage over .text [0x{lo:X}, 0x{hi:X}) : {words:N0} PC slots --");
    Console.WriteLine($"   executed instrs in window: {execTotal:N0}   distinct PCs touched: {distinct:N0}");
    if (execTotal == 0) { Console.WriteLine("   (no executed PCs landed in the window)"); return; }

    var sorted = pch.Where(c => c > 0).OrderByDescending(c => c).ToArray();
    double[] pct = { 50, 80, 90, 95, 99 };
    int pi = 0; ulong acc = 0;
    var hits = new int[pct.Length];
    for (int k = 0; k < sorted.Length && pi < pct.Length; k++)
    {
        acc += sorted[k];
        while (pi < pct.Length && acc * 100.0 >= pct[pi] * execTotal) hits[pi++] = k + 1;
    }
    while (pi < pct.Length) hits[pi++] = sorted.Length;   // 100% needs all distinct PCs

    Console.WriteLine("   distinct PCs covering N% of executed instructions:");
    for (int k = 0; k < pct.Length; k++)
        Console.WriteLine($"     {pct[k],3:F0}%  ->  {hits[k],8:N0} PCs  ({(distinct > 0 ? 100.0 * hits[k] / distinct : 0),5:F1}% of touched, {(words > 0 ? 100.0 * hits[k] / words : 0),5:F2}% of .text)");
}
