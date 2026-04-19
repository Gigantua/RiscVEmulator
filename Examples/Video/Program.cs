using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

// Resolve shared runtime files
string solutionRoot = Path.GetFullPath(Path.Combine(exeDir, "..", "..", "..", "..", ".."));
string runtimeDir   = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Runtime");
string runtimeC     = Path.Combine(runtimeDir, "runtime.c");
string libcC        = Path.Combine(runtimeDir, "libc.c");
string softfloatC   = Path.Combine(runtimeDir, "softfloat.c");
string syscallsC    = Path.Combine(runtimeDir, "syscalls.c");
string linkerLd     = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Programs", "linker.ld");
string demoC        = Path.Combine(exeDir, "Programs", "video_demo.c");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);

// ── Compile ──────────────────────────────────────────────────────
Console.WriteLine("Compiling video demo...");

string runtimeObj   = Path.Combine(buildDir, "runtime.o");
string libcObj      = Path.Combine(buildDir, "libc.o");
string softfloatObj = Path.Combine(buildDir, "softfloat.o");
string syscallsObj  = Path.Combine(buildDir, "syscalls.o");
string demoObj      = Path.Combine(buildDir, "video_demo.o");
string elfPath      = Path.Combine(buildDir, "video_demo.elf");

CompileObj(runtimeC, runtimeObj);
CompileObj(libcC, libcObj);
CompileObj(softfloatC, softfloatObj);
CompileObj(syscallsC, syscallsObj);
CompileObj(demoC, demoObj);
Link(new[] { demoObj, libcObj, softfloatObj, syscallsObj, runtimeObj }, elfPath);

Console.WriteLine($"  video_demo.elf: {new FileInfo(elfPath).Length:N0} bytes");

// ── Build SoC ────────────────────────────────────────────────────
var memory  = new Memory(4 * 1024 * 1024); // 4MB is plenty
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
var fb      = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
var kbd     = new KeyboardDevice();
var mouse   = new MouseDevice();
var rtc     = new RealTimeClockDevice();
var audioBuf  = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(fb);
bus.RegisterPeripheral(display);
bus.RegisterPeripheral(kbd);
bus.RegisterPeripheral(mouse);
bus.RegisterPeripheral(rtc);
bus.RegisterPeripheral(audioBuf);
bus.RegisterPeripheral(audioCtrl);

uart.OutputHandler = c => Console.Write(c);

byte[] elfData = File.ReadAllBytes(elfPath);
var regs = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, 0x003FFF00u); // stack near top of 4MB

var emu = new Emulator(bus, regs, entry);
emu.OutputHandler = c => Console.Write(c);

// ── Run ──────────────────────────────────────────────────────────
var opts = new SdlWindowOptions
{
    Title = "Video Demo — RV32I",
    GrabMouse = false,
    Scale = 3,
    TargetFps = 60,
};

var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts);
return window.Run();

// ── Helpers ──────────────────────────────────────────────────────
void CompileObj(string src, string obj)
{
    Console.Write($"  {Path.GetFileName(src)}... ");
    RunClang(new[] {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char", "-c",
        $"-I{runtimeDir}", src, "-o", obj
    });
    Console.WriteLine("OK");
}

void Link(string[] objs, string elf)
{
    Console.Write("  Linking... ");
    var linkArgs = new List<string> {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objs);
    linkArgs.AddRange(new[] { "-o", elf });
    RunClang(linkArgs.ToArray());
    Console.WriteLine("OK");
}

void RunClang(string[] clangArgs)
{
    var psi = new ProcessStartInfo(clang)
    {
        RedirectStandardError = true,
        RedirectStandardOutput = true,
        UseShellExecute = false,
    };
    foreach (var a in clangArgs)
        psi.ArgumentList.Add(a);
    var proc = Process.Start(psi)!;
    string stderr = proc.StandardError.ReadToEnd();
    proc.WaitForExit();
    if (proc.ExitCode != 0)
    {
        Console.Error.WriteLine($"FAILED");
        Console.Error.WriteLine(stderr);
        Environment.Exit(1);
    }
}
