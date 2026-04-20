using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

// Walk up from the exe dir to find the solution root (the directory containing Runtime/)
string? solutionRoot = exeDir;
while (solutionRoot != null && !Directory.Exists(Path.Combine(solutionRoot, "Runtime")))
    solutionRoot = Path.GetDirectoryName(solutionRoot);
if (solutionRoot == null)
    throw new DirectoryNotFoundException("Could not find solution root — no Runtime/ folder in any parent directory.");
string runtimeDir   = Path.Combine(solutionRoot, "Runtime");
string runtimeC     = Path.Combine(runtimeDir, "runtime.c");
string libcC        = Path.Combine(runtimeDir, "libc.c");
string softfloatC   = Path.Combine(runtimeDir, "softfloat.c");
string syscallsC    = Path.Combine(runtimeDir, "syscalls.c");
string crt0C        = Path.Combine(runtimeDir, "crt0.c");
string linkerLd     = Path.Combine(runtimeDir, "linker.ld");
string demoC        = Path.Combine(exeDir, "Programs", "midi_demo.c");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);

// ── Compile ──────────────────────────────────────────────────────
Console.WriteLine("Compiling MIDI demo...");

string runtimeObj   = Path.Combine(buildDir, "runtime.o");
string libcObj      = Path.Combine(buildDir, "libc.o");
string softfloatObj = Path.Combine(buildDir, "softfloat.o");
string syscallsObj  = Path.Combine(buildDir, "syscalls.o");
string crt0Obj      = Path.Combine(buildDir, "crt0.o");
string demoObj      = Path.Combine(buildDir, "midi_demo.o");
string elfPath      = Path.Combine(buildDir, "midi_demo.elf");

CompileObj(runtimeC, runtimeObj);
CompileObj(libcC, libcObj);
CompileObj(softfloatC, softfloatObj);
CompileObj(syscallsC, syscallsObj);
CompileObj(crt0C, crt0Obj);
CompileObj(demoC, demoObj);
Link(new[] { demoObj, crt0Obj, libcObj, softfloatObj, syscallsObj, runtimeObj }, elfPath);

Console.WriteLine($"  midi_demo.elf: {new FileInfo(elfPath).Length:N0} bytes");

// ── Build SoC ────────────────────────────────────────────────────
var memory  = new Memory(4 * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
using var midi = new MidiDevice();

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(midi);

uart.OutputHandler = c => Console.Write(c);

byte[] elfData = File.ReadAllBytes(elfPath);
var regs = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, 0x003FFF00u);

var emu = new Emulator(bus, regs, entry);
emu.OutputHandler = c => Console.Write(c);

// ── Run ──────────────────────────────────────────────────────────
Console.WriteLine("Starting MIDI demo...");

bool running = true;
var cpuThread = new System.Threading.Thread(() =>
{
    while (running && !emu.IsHalted)
    {
        for (int i = 0; i < 500_000 && running && !emu.IsHalted; i++)
            emu.Step();
    }
})
{ IsBackground = true, Name = "RV32I-CPU" };
cpuThread.Start();

// Wait for completion
cpuThread.Join();
Thread.Sleep(1500); // let the MIDI synth finish rendering the last notes
Console.WriteLine("MIDI demo finished.");

return 0;

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