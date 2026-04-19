using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

// ── Configuration ─────────────────────────────────────────────────
const uint StackPointer = 0x00EFFF00;
const int  RamMB        = 16;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";

string exeDir      = AppContext.BaseDirectory;
string programsDir = Path.Combine(exeDir, "Programs");

// Walk up from the output dir until we find the solution root (contains RiscVEmulator.sln)
string? solutionRoot = exeDir;
while (solutionRoot != null && !File.Exists(Path.Combine(solutionRoot, "RiscVEmulator.sln")))
    solutionRoot = Path.GetDirectoryName(solutionRoot);
if (solutionRoot == null)
    throw new DirectoryNotFoundException($"Cannot find RiscVEmulator.sln starting from {exeDir}");

string runtimeDir   = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Runtime");
string testProgDir  = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Programs");
string linkerLd     = Path.Combine(testProgDir,  "linker.ld");

// ── Parse CLI args ─────────────────────────────────────────────────
var opts = new SdlWindowOptions { Title = "Voxel — RV32I Emulator", GrabMouse = true };
bool enableMExt = true;
bool enableFExt = false;

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--scale":    opts.Scale     = int.Parse(args[++i]); break;
        case "--fps":      opts.TargetFps = int.Parse(args[++i]); break;
        case "--no-grab":  opts.GrabMouse = false; break;
        case "--no-m-ext": enableMExt = false; break;
        case "--f-ext":    enableFExt = true;  break;
    }
}

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);
string elfPath = Path.Combine(buildDir, "voxel.elf");

// ── Sources ────────────────────────────────────────────────────────
// voxel_main.c is self-contained: no OpenGL, no GLFW, no networking.
// It writes directly to the MMIO framebuffer at 0x20000000.
string voxelMain = Path.Combine(programsDir, "voxel_main.c");

string[] allSources =
{
    Path.Combine(runtimeDir, "crt0.c"),
    Path.Combine(runtimeDir, "runtime.c"),
    Path.Combine(runtimeDir, "softfloat.c"),
    Path.Combine(runtimeDir, "math.c"),
    Path.Combine(runtimeDir, "libc.c"),
    Path.Combine(runtimeDir, "syscalls.c"),
    Path.Combine(runtimeDir, "malloc.c"),
    Path.Combine(runtimeDir, "vfs.c"),
    voxelMain,
};

string[] includeDirs = { runtimeDir, programsDir };

// ── Compile ────────────────────────────────────────────────────────
Console.WriteLine($"Compiling Voxel for RV32I{(enableMExt?"+M":"")}{(enableFExt?"+F":"")}...");
var objFiles = new List<string>();

foreach (string src in allSources)
{
    if (!File.Exists(src))
    {
        Console.Error.WriteLine($"Source not found: {src}");
        return 1;
    }
    string objName = Path.GetFileNameWithoutExtension(src) + ".o";
    string objPath = Path.Combine(buildDir, objName);
    string[] extra = includeDirs.Select(d => $"-I{d}").ToArray();
    if (!Compile(src, objPath, extra, enableMExt, enableFExt))
        return 1;
    objFiles.Add(objPath);
}

// ── Link ───────────────────────────────────────────────────────────
{
    string march = enableFExt ? (enableMExt ? "rv32imf" : "rv32if")
                              : (enableMExt ? "rv32im"  : "rv32i");
    string mabi  = enableFExt ? "ilp32f" : "ilp32";
    var linkArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", $"-march={march}", $"-mabi={mabi}",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objFiles);
    linkArgs.Add("-o");
    linkArgs.Add(elfPath);

    Console.Write("  Linking... ");
    if (!RunClang(linkArgs.ToArray()))
        return 1;
    Console.WriteLine("OK");
}

Console.WriteLine($"  voxel.elf: {new FileInfo(elfPath).Length:N0} bytes");

// ── Build SoC ──────────────────────────────────────────────────────
Console.WriteLine("Building SoC...");
byte[] elfData = File.ReadAllBytes(elfPath);
var memory  = new Memory(RamMB * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
var fb      = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
var kbd     = new KeyboardDevice();
var mouse   = new MouseDevice();
var rtc     = new RealTimeClockDevice();
var audioBuf  = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();
var midi      = new MidiDevice();

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(fb);
bus.RegisterPeripheral(display);
bus.RegisterPeripheral(kbd);
bus.RegisterPeripheral(mouse);
bus.RegisterPeripheral(rtc);
bus.RegisterPeripheral(audioBuf);
bus.RegisterPeripheral(audioCtrl);
bus.RegisterPeripheral(midi);

uart.OutputHandler = c => Console.Write(c);

var regs   = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, StackPointer);

var emu = new Emulator(bus, regs, entry);
emu.EnableMExtension = enableMExt;
emu.EnableFExtension = enableFExt;
emu.OutputHandler = c => Console.Write(c);

Console.WriteLine("Starting Voxel...");
var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts, midi);
return window.Run();

// ── Helpers ────────────────────────────────────────────────────────
bool Compile(string src, string obj, string[] extraFlags, bool mExt, bool fExt)
{
    Console.Write($"  {Path.GetFileName(src)}... ");
    string march = fExt ? (mExt ? "rv32imf" : "rv32if")
                        : (mExt ? "rv32im"  : "rv32i");
    string mabi  = fExt ? "ilp32f" : "ilp32";
    var compileArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", $"-march={march}", $"-mabi={mabi}",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char", "-c",
    };
    compileArgs.AddRange(extraFlags);
    compileArgs.Add(src);
    compileArgs.Add("-o");
    compileArgs.Add(obj);
    if (!RunClang(compileArgs.ToArray()))
        return false;
    Console.WriteLine("OK");
    return true;
}

bool RunClang(string[] clangArgs)
{
    var psi = new ProcessStartInfo(clang)
    {
        RedirectStandardError  = true,
        RedirectStandardOutput = true,
        UseShellExecute        = false,
    };
    foreach (var a in clangArgs)
        psi.ArgumentList.Add(a);

    var proc   = Process.Start(psi)!;
    string err = proc.StandardError.ReadToEnd();
    proc.WaitForExit();

    if (proc.ExitCode != 0)
    {
        Console.Error.WriteLine($"FAILED (exit {proc.ExitCode})");
        Console.Error.WriteLine(err);
        return false;
    }
    return true;
}
