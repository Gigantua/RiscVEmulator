using System.Diagnostics;
using RiscVEmulator.Core.Cuda;
using RiscVEmulator.Frontend;

// ── DOOM on the GPU RV32I core ────────────────────────────────────────
// Same PureDOOM guest as Examples/Doom, but the CPU runs on the GPU via
// CudaEmulator. Default: one instance, played in an SDL window ("CUDA runs
// Doom"). `--cores N`: run N independent Doom instances headless and report
// AGGREGATE MIPS — the throughput win (a single GPU thread is latency-bound at
// ~2.6 MIPS, but N of them scale far past that).

const uint WadBaseAddr = 0x00A00000;
const uint WadSizeAddr = 0x009FFFFC;
const uint StackPointer = 0x009FFF00;
const int  RamMB = 16;

int cores = 1;
bool selftest = false;
string? shotPath = null;
int shotFrame = 10;
var opts = new SdlWindowOptions { Title = "DOOM — RV32I on CUDA", GrabMouse = true };
for (int i = 0; i < args.Length; i++)
{
    if      (args[i] == "--cores")    cores = int.Parse(args[++i]);
    else if (args[i] == "--selftest") selftest = true;
    else if (args[i] == "--shot")     shotPath = args[++i];                 // headless: save frame N as PNG
    else if (args[i] == "--shot-frame") shotFrame = int.Parse(args[++i]);
    else if (args[i] == "--scale")    opts.Scale = int.Parse(args[++i]);
    else if (args[i] == "--no-grab")  opts.GrabMouse = false;
}

string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;
string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln"))) root = Path.GetDirectoryName(root);
if (root == null) throw new DirectoryNotFoundException("RiscVEmulator.sln not found");

string runtimeDir  = Path.Combine(root, "Runtime");
string linkerLd    = Path.Combine(runtimeDir, "linker.ld");
string programsDir = Path.Combine(root, "Examples", "Doom", "Programs");   // reuse PureDOOM guest
string wadPath     = Path.Combine(exeDir, "doom1.wad");
string buildDir    = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);
string elfPath     = Path.Combine(buildDir, "doom.elf");

if (!File.Exists(wadPath)) { Console.Error.WriteLine($"WAD not found: {wadPath}"); return 1; }

Console.WriteLine("Compiling DOOM for RV32IMA...");
string[] runtimeSrcs = { "runtime.c", "softfloat.c", "libc.c", "syscalls.c", "malloc.c", "vfs.c" };
var objs = new List<string>();
foreach (string s in runtimeSrcs)
{
    string src = Path.Combine(runtimeDir, s);
    string obj = Path.Combine(buildDir, Path.GetFileNameWithoutExtension(s) + ".o");
    if (!Compile(src, obj, new[] { $"-I{runtimeDir}" })) return 1;
    objs.Add(obj);
}
{
    string obj = Path.Combine(buildDir, "doom_main.o");
    if (!Compile(Path.Combine(programsDir, "doom_main.c"), obj,
                 new[] { $"-I{programsDir}", $"-I{runtimeDir}" })) return 1;
    objs.Add(obj);
}
if (!Clang(new[] { "--target=riscv32-unknown-elf","-march=rv32ima","-mabi=ilp32",
                   "-nostdlib","-nostartfiles","-O3","-fno-builtin","-fsigned-char",
                   "-fuse-ld=lld", $"-Wl,-T,{linkerLd}" }
           .Concat(objs).Concat(new[] { "-o", elfPath }).ToArray())) return 1;
Console.WriteLine($"  doom.elf: {new FileInfo(elfPath).Length:N0} bytes");

byte[] elfData = File.ReadAllBytes(elfPath);
byte[] wadData = File.ReadAllBytes(wadPath);

if (args.Contains("--bench"))
{
    // Steady-state DOOM MIPS: warm up to the first rendered frame (skip cold
    // init), then time a fixed number of guest steps in-game on the interpreter.
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer); emu.SetEntry(entry);
    Console.WriteLine("DOOM bench on interpreter (warming PAST the static title into demo playback)...");
    static ulong FbHash(byte[] px)
    {
        ulong h = 1469598103934665603UL;
        for (int i = 0; i + 3 < px.Length; i += 4)
        { h = (h ^ px[i]) * 1099511628211UL; h = (h ^ px[i + 1]) * 1099511628211UL; h = (h ^ px[i + 2]) * 1099511628211UL; }
        return h;
    }
    // Warm until the framebuffer is actually CHANGING (demo rendering, not the
    // static title where DOOM just spins on the timer doing no real work).
    long warm = 0; int wf = 0; ulong lastH = 0;
    for (int b = 0; b < 250 && !emu.IsHalted; b++)
    {
        emu.StepN(1_000_000); warm += 1_000_000;
        ulong hsh = FbHash(emu.Framebuffer.PresentedPixels);
        if (hsh != lastH) { lastH = hsh; wf++; }
        if (wf >= 20) break;
    }
    // Measure: time to render a batch of demo frames; report MIPS + FPS (FPS is
    // the honest DOOM metric — render work, not timer spin).
    var sw = Stopwatch.StartNew();
    long m = 0; int mf = 0; lastH = FbHash(emu.Framebuffer.PresentedPixels);
    for (int b = 0; b < 4000 && !emu.IsHalted; b++)
    {
        emu.StepN(2_000_000); m += 2_000_000;
        ulong hsh = FbHash(emu.Framebuffer.PresentedPixels);
        if (hsh != lastH) { lastH = hsh; mf++; }
        if (mf >= 30) break;
    }
    sw.Stop();
    double sec = sw.Elapsed.TotalSeconds;
    Console.WriteLine($"  warm={warm / 1_000_000}M steps ({wf} frames); measured {m / 1_000_000}M steps, " +
                      $"{mf} frames in {sec:F2}s");
    Console.WriteLine($"  => {m / sec / 1e6:F2} MIPS, {mf / sec:F2} FPS  [interpreter]");
    return 0;
}

if (args.Contains("--isaprof"))
{
    // ── Dynamic ISA profile of DOOM: which opcodes dominate, which instruction
    //    follows which, and how often the fusible idioms (li/la/GOT) actually
    //    occur — to focus optimization on the hot instructions/pairs. ──
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer); emu.SetEntry(entry);
    Console.WriteLine("isaprof: warming into gameplay...");
    for (int b = 0; b < 40 && !emu.IsHalted; b++) emu.StepN(1_000_000);   // past init into render
    Console.WriteLine("isaprof: profiling 40M steps (TDR-safe chunks)...");
    emu.ProfReset();
    for (int b = 0; b < 20 && !emu.IsHalted; b++) emu.Profile(2_000_000);
    ulong[] p = emu.ProfRead();

    string[] name = new string[32];
    for (int i = 0; i < 32; i++) name[i] = $"op{i*4+3:X2}";
    name[0]="LOAD"; name[3]="FENCE"; name[4]="OP-IMM"; name[5]="AUIPC"; name[8]="STORE";
    name[11]="AMO"; name[12]="OP"; name[13]="LUI"; name[24]="BRANCH"; name[25]="JALR"; name[27]="JAL"; name[28]="SYSTEM";

    ulong total = p[1027]; if (total == 0) { Console.Error.WriteLine("no steps profiled"); return 1; }
    var op = new ulong[32];
    var pairs = new List<(ulong c, int a, int b)>();
    for (int a = 0; a < 32; a++) for (int b = 0; b < 32; b++) { ulong c = p[a*32+b]; op[a]+=c; if (c>0) pairs.Add((c,a,b)); }
    Console.WriteLine($"\n== Dynamic opcode mix ({total/1e6:F0}M instrs) ==");
    foreach (var (c,i) in op.Select((c,i)=>(c,i)).OrderByDescending(x=>x.c).Take(12))
        if (c>0) Console.WriteLine($"  {name[i],-8} {100.0*c/total,6:F2}%  ({c/1e6:F1}M)");
    Console.WriteLine("\n== Top adjacent pairs (prev -> cur) ==");
    foreach (var (c,a,b) in pairs.OrderByDescending(x=>x.c).Take(15))
        Console.WriteLine($"  {name[a],-8} -> {name[b],-8} {100.0*c/total,6:F2}%  ({c/1e6:F1}M)");
    Console.WriteLine("\n== Fusible idioms (exact, same-reg) ==");
    Console.WriteLine($"  LUI+ADDI (li)   {100.0*p[1024]/total,6:F2}%  ({p[1024]/1e6:F1}M)");
    Console.WriteLine($"  AUIPC+ADDI (la) {100.0*p[1025]/total,6:F2}%  ({p[1025]/1e6:F1}M)");
    Console.WriteLine($"  AUIPC+LW (GOT)  {100.0*p[1026]/total,6:F2}%  ({p[1026]/1e6:F1}M)");
    return 0;
}

if (args.Contains("--prof"))
{
    // ── Nsight Compute harness: warm into representative DOOM code, then issue a
    //    few steady single-core launches and exit immediately, so ncu can profile
    //    one steady kernel without dragging through all ~170 cold-boot launches.
    //    Run: ncu --launch-skip 15 --launch-count 1 ... CudaDoom.dll --prof ──
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer); emu.SetEntry(entry);
    Console.WriteLine("prof: warming 15 launches then 3 steady launches (single core)...");
    for (int b = 0; b < 15 && !emu.IsHalted; b++) emu.StepN(2_000_000);   // warm to past init
    for (int b = 0; b < 3 && !emu.IsHalted; b++)  emu.StepN(2_000_000);   // steady — profile one of these
    Console.WriteLine("prof: done");
    return 0;
}

if (args.Contains("--ttf"))
{
    // ── DOOM time-to-first-N-frames (cold boot) — the honest "jumpy code" MIPS
    //    metric. Unlike the tight compute/data loops in CudaBench, DOOM's cold
    //    init + early render is branch- and indirection-heavy (WAD parsing, setup,
    //    the first real render), so this is representative of actual guest code.
    //    A microbench can look 190x faster yet be slower here — this is the number
    //    to optimize. Reports M steps, wall time and MIPS to the first N distinct
    //    non-black frames. `--ttf N` sets N (default 2). ──
    int want = 2;
    int fi = Array.IndexOf(args, "--ttf");
    if (fi >= 0 && fi + 1 < args.Length && int.TryParse(args[fi + 1], out int wn)) want = wn;

    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer); emu.SetEntry(entry);

    Console.WriteLine($"DOOM time-to-first-{want}-frames (cold boot, branchy/indirection-heavy):");
    const int Batch = 200_000;        // small batches → tight time-to-frame, launch overhead is counted (realistic)
    var sw = Stopwatch.StartNew();
    long steps = 0; int frames = 0; ulong last = 0;
    for (int b = 0; b < 20000 && !emu.IsHalted; b++)
    {
        emu.StepN(Batch); steps += Batch;
        var px = emu.Framebuffer.PresentedPixels;
        ulong h = 1469598103934665603UL; int nz = 0;
        for (int i = 0; i + 3 < px.Length; i += 4)
        {
            if ((px[i] | px[i + 1] | px[i + 2]) != 0) nz++;
            h = (h ^ px[i]) * 1099511628211UL; h = (h ^ px[i + 1]) * 1099511628211UL; h = (h ^ px[i + 2]) * 1099511628211UL;
        }
        if (nz > 5000 && h != last)
        {
            last = h; frames++;
            double s = sw.Elapsed.TotalSeconds;
            Console.WriteLine($"  frame {frames}: {steps / 1e6:F1}M steps, {s:F2}s, {steps / s / 1e6:F2} MIPS");
            if (frames >= want)
            {
                Console.WriteLine($"=> time-to-{want}-frames: {steps / 1e6:F1}M steps in {s:F2}s = {steps / s / 1e6:F2} MIPS");
                return 0;
            }
        }
    }
    Console.Error.WriteLine($"only reached {frames}/{want} frames after {steps / 1e6:F0}M steps"); return 1;
}

if (shotPath != null)
{
    // ── Headless: run until DOOM has presented `shotFrame` distinct frames, then
    //    save the framebuffer as a PNG so the render can be eyeballed. ──
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer);
    emu.SetEntry(entry);
    const int W = 320, H = 200;
    Console.WriteLine($"Headless: rendering until frame {shotFrame} ({W}x{H}) on interpreter...");
    int frames = 0; long steps = 0; ulong last = 0;
    for (int b = 0; b < 4000 && !emu.IsHalted; b++)
    {
        emu.StepN(500_000); steps += 500_000;
        var px = emu.Framebuffer.PresentedPixels;
        ulong h = 1469598103934665603UL; int nz = 0;
        for (int i = 0; i + 3 < px.Length; i += 4)
        {
            if ((px[i] | px[i + 1] | px[i + 2]) != 0) nz++;
            h = (h ^ px[i]) * 1099511628211UL;
            h = (h ^ px[i + 1]) * 1099511628211UL;
            h = (h ^ px[i + 2]) * 1099511628211UL;
        }
        if (nz > 5000 && h != last) { last = h; frames++; }
        Console.Write($"\r[{steps / 1_000_000}M steps] presented frames={frames} non-black px={nz}   ");
        if (frames >= shotFrame)
        {
            Png.WriteRgba(shotPath, emu.Framebuffer.PresentedPixels, W, H);
            Console.WriteLine($"\nwrote {shotPath} (frame {frames}, {nz} non-black px, {steps / 1_000_000}M steps).");
            return 0;
        }
    }
    Console.Error.WriteLine($"\nonly reached {frames} frames"); return 1;
}

if (selftest)
{
    // ── Headless: run one instance until DOOM actually renders a frame ──
    // (Measured: the shared instruction window REGRESSES single-core — the hot
    //  code is already L1-resident, so it adds bookkeeping for no latency win.
    //  Left off; single-guest speed is interpreter-bound, see CudaBench.)
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = c => Console.Write(c);
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer);
    emu.SetEntry(entry);
    Console.WriteLine("\nSelf-test: stepping until the DOOM framebuffer is non-blank...");
    var sw = Stopwatch.StartNew();
    long steps = 0;
    // 1 M steps/launch keeps each kernel well under the WDDM TDR window.
    for (int b = 0; b < 1000 && !emu.IsHalted; b++)
    {
        emu.StepN(1_000_000); steps += 1_000_000;
        var px = emu.Framebuffer.PresentedPixels;
        int nz = 0; for (int i = 0; i + 3 < px.Length; i += 4) if ((px[i] | px[i+1] | px[i+2]) != 0) nz++;
        if (nz > 5000)
        {
            Console.WriteLine($"\nRENDERED: DOOM framebuffer non-blank ({nz} px) after " +
                              $"{steps/1_000_000}M steps, {sw.Elapsed.TotalSeconds:F0}s " +
                              $"({steps/sw.Elapsed.TotalSeconds/1e6:F2} MIPS).");
            return 0;
        }
        Console.Write($"\r[{steps/1_000_000}M steps, {sw.Elapsed.TotalSeconds:F0}s] fb non-black px = {nz}   ");
    }
    Console.Error.WriteLine($"\nstill blank after {steps/1_000_000}M steps");
    return 1;
}

if (cores <= 1)
{
    // ── Single instance: play it ── (prefetch window left off — see selftest note)
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.OutputHandler = c => Console.Write(c);
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer);
    emu.SetEntry(entry);
    Console.WriteLine($"  WAD: {wadData.Length:N0} bytes @ 0x{WadBaseAddr:X8}");
    Console.WriteLine("Starting DOOM on the CUDA core (single GPU thread).");
    var window = new SdlWindow(emu.Framebuffer, emu.Display, emu.Keyboard, emu.Mouse,
                               emu.AudioBuffer, emu.AudioControl, emu, opts, emu.Midi);
    return window.Run();
}

// ── Many instances: aggregate throughput ──
// Guard VRAM: each Doom instance needs ~16 MB. On a display GPU, oversubscribing
// VRAM thrashes managed-memory migration and can hard-lock the machine, so cap
// total guest RAM to ~2 GB and refuse larger requests with a clear message.
long totalRamMB = (long)cores * RamMB;
if (totalRamMB > 2048)
{
    Console.Error.WriteLine($"Refusing {cores} instances = {totalRamMB} MB guest RAM " +
        $"(> 2 GB). That risks oversubscribing VRAM on a display GPU and hanging the " +
        $"machine. Use --cores {2048 / RamMB} or fewer.");
    return 1;
}
Console.WriteLine($"Running {cores} DOOM instances headless for aggregate MIPS...");
using (var emu = new CudaEmulator(RamMB * 1024 * 1024, nCores: cores))
{
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImageToAllCores(StackPointer, entry);

    // Cap per-launch budget so a kernel stays well under the TDR window.
    long budget = Math.Clamp(20_000_000L / cores, 50_000L, 250_000L);
    emu.StepN((int)budget);                  // warm-up (Doom init + GPU clock ramp)
    var sw = Stopwatch.StartNew();
    int iters = 4;
    for (int i = 0; i < iters; i++) emu.StepN((int)budget);
    sw.Stop();
    double aggMips = (double)cores * budget * iters / sw.Elapsed.TotalSeconds / 1e6;
    Console.WriteLine($"  {cores} instances: {aggMips:F0} MIPS aggregate " +
                      $"({aggMips / cores:F2} MIPS/instance) — vs ~2.6 single.");
}
return 0;

// ── clang helpers (PureDOOM.h needs the warning suppressions) ──
bool Compile(string src, string obj, string[] extra)
{
    Console.Write($"  {Path.GetFileName(src)}... ");
    var a = new List<string> {
        "--target=riscv32-unknown-elf","-march=rv32ima","-mabi=ilp32",
        "-nostdlib","-O3","-fno-builtin","-fsigned-char","-c",
        "-Wno-unused-command-line-argument","-Wno-deprecated-non-prototype",
        "-Wno-parentheses","-Wno-enum-compare",
    };
    a.AddRange(extra); a.Add(src); a.Add("-o"); a.Add(obj);
    if (!Clang(a.ToArray())) return false;
    Console.WriteLine("OK");
    return true;
}
bool Clang(string[] a)
{
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var x in a) psi.ArgumentList.Add(x);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    if (p.ExitCode != 0) { Console.Error.WriteLine($"\nclang failed ({p.ExitCode})\n{err}"); return false; }
    return true;
}

// Minimal PNG writer (RGBA8888, no deps beyond built-in ZLibStream).
static class Png
{
    public static void WriteRgba(string path, byte[] rgba, int w, int h)
    {
        using var fs = File.Create(path);
        fs.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });
        byte[] ihdr = new byte[13];
        BE(ihdr, 0, (uint)w); BE(ihdr, 4, (uint)h);
        ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; // 8-bit RGBA
        Chunk(fs, "IHDR", ihdr);
        byte[] raw = new byte[h * (1 + w * 4)];
        int o = 0;
        for (int y = 0; y < h; y++) { raw[o++] = 0; Array.Copy(rgba, y * w * 4, raw, o, w * 4); o += w * 4; }
        byte[] idat;
        using (var ms = new MemoryStream())
        {
            using (var z = new System.IO.Compression.ZLibStream(ms, System.IO.Compression.CompressionLevel.Optimal, true))
                z.Write(raw, 0, raw.Length);
            idat = ms.ToArray();
        }
        Chunk(fs, "IDAT", idat);
        Chunk(fs, "IEND", Array.Empty<byte>());
    }
    static void Chunk(Stream s, string type, byte[] data)
    {
        byte[] len = new byte[4]; BE(len, 0, (uint)data.Length); s.Write(len);
        byte[] t = System.Text.Encoding.ASCII.GetBytes(type); s.Write(t); s.Write(data);
        byte[] c = new byte[4]; BE(c, 0, Crc(t, data)); s.Write(c);
    }
    static void BE(byte[] b, int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
    static readonly uint[] T = Build();
    static uint[] Build() { var t = new uint[256]; for (uint n = 0; n < 256; n++) { uint c = n; for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); t[n] = c; } return t; }
    static uint Crc(byte[] a, byte[] b) { uint c = 0xFFFFFFFFu; foreach (var x in a) c = T[(c ^ x) & 0xFF] ^ (c >> 8); foreach (var x in b) c = T[(c ^ x) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
}
