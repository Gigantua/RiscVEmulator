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
bool useJit = false;
bool usePtxJit = false;
string? shotPath = null;
int shotFrame = 10;
var opts = new SdlWindowOptions { Title = "DOOM — RV32I on CUDA", GrabMouse = true };
for (int i = 0; i < args.Length; i++)
{
    if      (args[i] == "--cores")    cores = int.Parse(args[++i]);
    else if (args[i] == "--selftest") selftest = true;
    else if (args[i] == "--jit")      useJit = true;
    else if (args[i] == "--ptxjit")   { useJit = true; usePtxJit = true; }  // in-process driver-API JIT
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

if (shotPath != null)
{
    // ── Headless: run until DOOM has presented `shotFrame` distinct frames, then
    //    save the framebuffer as a PNG so the render can be eyeballed. Uses the
    //    selected CPU backend (--ptxjit / --jit / interpreter). ──
    using var emu = new CudaEmulator(RamMB * 1024 * 1024);
    emu.UseJit = useJit; emu.UsePtxJit = usePtxJit;
    emu.OutputHandler = _ => { };
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer);
    emu.SetEntry(entry);
    const int W = 320, H = 200;
    Console.WriteLine($"Headless: rendering until frame {shotFrame} ({W}x{H}) on " +
                      (usePtxJit ? "ptx/driver-API JIT" : useJit ? "nvcc-DLL JIT" : "interpreter") + "...");
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
    emu.UseJit = useJit;                 // --jit: run the native-CUDA JIT'd guest
    emu.UsePtxJit = usePtxJit;           // --ptxjit: in-process driver-API JIT (cubin)
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
    emu.UseJit = useJit;                 // --jit: run the native-CUDA JIT'd guest
    emu.UsePtxJit = usePtxJit;           // --ptxjit: in-process driver-API JIT (cubin)
    emu.OutputHandler = c => Console.Write(c);
    uint entry = emu.LoadElf(elfData);
    emu.LoadBytes(WadSizeAddr, BitConverter.GetBytes((uint)wadData.Length));
    emu.LoadBytes(WadBaseAddr, wadData);
    emu.CommitImage();
    emu.SetReg(2, StackPointer);
    emu.SetEntry(entry);
    Console.WriteLine($"  WAD: {wadData.Length:N0} bytes @ 0x{WadBaseAddr:X8}");
    Console.WriteLine(useJit
        ? $"Starting DOOM on the CUDA JIT core (JIT {(emu.JitActive ? "active" : "FAILED → interpreter")})."
        : "Starting DOOM on the CUDA core (single GPU thread ~2.6 MIPS → a few FPS).");
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
    emu.UseSharedCode = true;                // coalesced, L2-resident fetch
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
