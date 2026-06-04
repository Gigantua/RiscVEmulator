using System.Diagnostics;
using RiscVEmulator.Core.Cuda;
using RiscVEmulator.Frontend;

// ── Framebuffer demo on the GPU RV32I core ────────────────────────────
// A tiny multiply-free guest animates a plasma straight into the MMIO
// framebuffer. Default: open the SDL window (you should see colour moving
// within a second). `--selftest`: render headless and assert the framebuffer
// is non-blank + animating — a fast, automated proof the FB path works on CUDA.

const uint Sp = 0x00EFFF00;
const int  RamMB = 16;
bool selftest = args.Contains("--selftest");
string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
    root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("RiscVEmulator.sln not found"); return 2; }

string src = Path.Combine(root, "Examples_CUDA", "CudaGfx", "Programs", "gfx_demo.c");
string buildDir = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);
string elf = Path.Combine(buildDir, "gfx_demo.elf");

Console.WriteLine("Compiling gfx_demo.c for RV32I...");
if (!Clang(new[] {
        "--target=riscv32-unknown-elf", "-march=rv32ima", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O2", "-fno-builtin", "-ffreestanding",
        "-fuse-ld=lld", "-Wl,-e,_start", "-Wl,--image-base=0x1000",
        src, "-o", elf })) return 2;

using var emu = new CudaEmulator(RamMB * 1024 * 1024);
uint entry = emu.LoadElf(File.ReadAllBytes(elf));
emu.CommitImage();
emu.SetReg(2, Sp);
emu.SetEntry(entry);

if (selftest)
{
    Console.WriteLine("Self-test: rendering headless...");
    uint? firstHash = null;
    for (int batch = 0; batch < 40; batch++)
    {
        emu.StepN(2_000_000);
        var px = emu.Framebuffer.PresentedPixels;
        var colors = new HashSet<uint>();
        int nonzero = 0; uint hash = 2166136261u;
        for (int i = 0; i + 3 < px.Length; i += 4)
        {
            uint c = (uint)(px[i] | px[i+1]<<8 | px[i+2]<<16);
            if (c != 0) nonzero++;
            colors.Add(c);
            hash = (hash ^ px[i]) * 16777619u;
        }
        if (nonzero > 2000 && colors.Count > 64)
        {
            if (firstHash == null) { firstHash = hash; }
            else if (hash != firstHash)   // a later frame differs → it's animating
            {
                Console.WriteLine($"PASS: framebuffer renders and animates " +
                                  $"({nonzero} non-black px, {colors.Count} distinct colors)");
                return 0;
            }
        }
    }
    Console.Error.WriteLine("FAIL: framebuffer did not render/animate");
    return 1;
}

Console.WriteLine("Framebuffer plasma on the CUDA core. Esc/Alt+F4 to exit.");
var opts = new SdlWindowOptions { Title = "CUDA framebuffer demo", GrabMouse = false };
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
