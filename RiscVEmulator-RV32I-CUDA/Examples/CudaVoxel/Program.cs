using System.Diagnostics;
using RiscVEmulator.Core.Cuda;
using RiscVEmulator.Frontend;

// ── Voxel terrain renderer on the GPU RV32I core ──────────────────────
// Same self-contained guest as Examples/Voxel (writes the MMIO framebuffer
// at 0x20000000, uses vsync, keyboard/mouse for the camera), but the CPU runs
// on the GPU via CudaEmulator. Default: open the SDL window. `--headless`:
// run without a window and report when the framebuffer first renders — Voxel
// is softfloat-heavy, so world-gen + first frame take a long time on one GPU
// thread (see Examples/CudaGfx for a fast, visibly-rendering FB demo).

const uint StackPointer = 0x00EFFF00;
const int  RamMB = 16;
bool headless = args.Contains("--headless");
bool useRvcud = !args.Contains("--no-jit");   // rvcud uop core + exec_block JIT (default ON; --no-jit = base kernel)
int benchN = 0;                               // --bench N: headless, run to N guest VSYNC presents, report MIPS + fps
string? shotPath = null;                      // --shot <path>: with --bench, save the final presented frame as PNG
{
    int bi = Array.IndexOf(args, "--bench");
    if (bi >= 0 && bi + 1 < args.Length && int.TryParse(args[bi + 1], out int bn)) benchN = bn;
    int si = Array.IndexOf(args, "--shot");
    if (si >= 0 && si + 1 < args.Length) shotPath = args[si + 1];
}
string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

var opts = new SdlWindowOptions { Title = "Voxel — RV32I on CUDA", GrabMouse = true };
for (int i = 0; i < args.Length; i++)
{
    if (args[i] == "--scale") opts.Scale = int.Parse(args[++i]);
    else if (args[i] == "--no-grab") opts.GrabMouse = false;
}

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
    root = Path.GetDirectoryName(root);
if (root == null) throw new DirectoryNotFoundException("RiscVEmulator.sln not found");

string runtimeDir  = Path.Combine(root, "Runtime");
string linkerLd    = Path.Combine(runtimeDir, "linker.ld");
string programsDir = Path.Combine(root, "Examples", "Voxel", "Programs");
string buildDir    = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);
string elfPath     = Path.Combine(buildDir, "voxel.elf");

string[] sources =
{
    Path.Combine(runtimeDir, "crt0.c"),     Path.Combine(runtimeDir, "runtime.c"),
    Path.Combine(runtimeDir, "softfloat.c"),Path.Combine(runtimeDir, "math.c"),
    Path.Combine(runtimeDir, "libc.c"),     Path.Combine(runtimeDir, "syscalls.c"),
    Path.Combine(runtimeDir, "malloc.c"),   Path.Combine(runtimeDir, "vfs.c"),
    Path.Combine(programsDir, "voxel_main.c"),
};
string[] inc = { $"-I{runtimeDir}", $"-I{programsDir}" };

Console.WriteLine("Compiling Voxel for RV32I...");
var objs = new List<string>();
foreach (string src in sources)
{
    if (!File.Exists(src)) { Console.Error.WriteLine($"missing {src}"); return 1; }
    string obj = Path.Combine(buildDir, Path.GetFileNameWithoutExtension(src) + ".o");
    if (!Clang(new[] { "--target=riscv32-unknown-elf","-march=rv32i","-mabi=ilp32",
                       "-nostdlib","-nostartfiles","-O3","-fno-builtin","-fsigned-char","-c" }
               .Concat(inc).Concat(new[]{ src, "-o", obj }).ToArray())) return 1;
    objs.Add(obj);
}
if (!Clang(new[] { "--target=riscv32-unknown-elf","-march=rv32i","-mabi=ilp32",
                   "-nostdlib","-nostartfiles","-O3","-fno-builtin","-fsigned-char",
                   "-fuse-ld=lld", $"-Wl,-T,{linkerLd}" }
           .Concat(objs).Concat(new[]{ "-o", elfPath }).ToArray())) return 1;
Console.WriteLine($"  voxel.elf: {new FileInfo(elfPath).Length:N0} bytes");

using var emu = new CudaEmulator(RamMB * 1024 * 1024);
emu.OutputHandler = c => Console.Write(c);
uint entry = emu.LoadElf(File.ReadAllBytes(elfPath));
if (useRvcud) Console.WriteLine("rvcud: cross-compiling the guest to the CUDA uop core + exec_block JIT.");
emu.UseRvcud = useRvcud;
emu.CommitImage();
emu.SetReg(2, StackPointer);
emu.SetEntry(entry);

if (benchN > 0)
{
    // Headless throughput bench (h4-style): frames = the guest's own VSYNC presents, steps =
    // actually-retired instructions. Reports MIPS over the span and the presented frame rate.
    Console.WriteLine($"Bench: running to {benchN} guest presents...");
    var sw = Stopwatch.StartNew();
    double tFirst = 0; ulong sFirst = 0;
    for (int b = 0; b < 100000 && !emu.IsHalted; b++)
    {
        emu.StepN(1_000_000);
        ulong vf = emu.Display.VsyncCount;
        if (vf >= 1 && tFirst == 0) { tFirst = sw.Elapsed.TotalSeconds; sFirst = emu.ActualSteps; }
        if (vf >= (ulong)benchN)
        {
            double s = sw.Elapsed.TotalSeconds, steps = emu.ActualSteps;
            double sSpan = s - tFirst;
            Console.WriteLine($"first present: {sFirst / 1e6:F1}M steps, {tFirst:F2}s");
            Console.WriteLine($"=> bench-{benchN}-presents: {steps / 1e6:F1}M steps in {s:F2}s = {steps / s / 1e6:F2} MIPS, " +
                              $"{(benchN - 1) / Math.Max(sSpan, 1e-9):F1} fps after first present");
            if (shotPath != null)
            {
                Png.WriteRgba(shotPath, emu.Framebuffer.PresentedPixels, 320, 200);
                Console.WriteLine($"wrote {shotPath}");
            }
            return 0;
        }
    }
    Console.Error.WriteLine($"only reached {emu.Display.VsyncCount}/{benchN} presents"); return 1;
}

if (headless)
{
    Console.WriteLine("Headless: stepping until the framebuffer first renders...");
    var sw = Stopwatch.StartNew();
    long steps = 0;
    const long Batch = 5_000_000, Cap = 4_000_000_000;
    while (!emu.IsHalted && steps < Cap)
    {
        emu.StepN((int)Batch); steps += Batch;
        var px = emu.Framebuffer.PresentedPixels;
        int nonzero = 0;
        for (int i = 0; i + 3 < px.Length; i += 4)
            if ((px[i] | px[i+1] | px[i+2]) != 0) nonzero++;
        if (nonzero > 2000)
        {
            Console.WriteLine($"\nRENDERED: framebuffer non-blank ({nonzero} px) after " +
                              $"{steps / 1_000_000}M steps, {sw.Elapsed.TotalSeconds:F0}s");
            return 0;
        }
        Console.Write($"\r[{steps / 1_000_000}M steps, {sw.Elapsed.TotalSeconds:F0}s] fb non-black px = {nonzero}   ");
    }
    Console.WriteLine($"\nstill blank after {steps / 1_000_000}M steps (halted={emu.IsHalted})");
    return 1;
}

Console.WriteLine("Starting Voxel (CUDA core)...  Esc/Alt+F4 to exit.");
if (!useRvcud)
{
    Console.WriteLine("Note: --no-jit runs the base kernel; a single GPU thread is slow for this");
    Console.WriteLine("softfloat-heavy guest — world generation + first frame take a while.");
}
var window = new SdlWindow(emu.Framebuffer, emu.Display, emu.Keyboard, emu.Mouse,
                           emu.AudioBuffer, emu.AudioControl, emu, opts, emu.Midi);
return window.Run();

bool Clang(string[] a)
{
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var x in a) psi.ArgumentList.Add(x);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    if (p.ExitCode != 0) { Console.Error.WriteLine($"clang failed ({p.ExitCode})\n{err}"); return false; }
    return true;
}

// Minimal PNG writer (RGBA8888, no deps beyond built-in ZLibStream) — same as CudaDoom's.
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
