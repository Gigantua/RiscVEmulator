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
emu.CommitImage();
emu.SetReg(2, StackPointer);
emu.SetEntry(entry);

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
Console.WriteLine("Note: a single GPU thread is slow for this softfloat-heavy guest;");
Console.WriteLine("world generation + first frame take a while (see CudaGfx for a fast demo).");
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
