using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

// ── Configuration ─────────────────────────────────────────────────
const uint WadBaseAddr = 0x00A00000;
const uint WadSizeAddr = 0x009FFFFC;
const uint StackPointer = 0x009FFF00;
const int RamMB = 16;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";

// Resolve paths relative to the executable
string exeDir = AppContext.BaseDirectory;
string doomMainC   = Path.Combine(exeDir, "Programs", "doom_main.c");
string pureDoomH   = Path.Combine(exeDir, "Programs"); // -I for PureDOOM.h
string wadPath     = Path.Combine(exeDir, "doom1.wad");

string solutionRoot = Path.GetFullPath(Path.Combine(exeDir, "..", "..", "..", "..", ".."));
string runtimeDir   = Path.Combine(solutionRoot, "Runtime");
string linkerLd     = Path.Combine(runtimeDir, "linker.ld");

string runtimeC   = Path.Combine(runtimeDir, "runtime.c");
string softfloatC = Path.Combine(runtimeDir, "softfloat.c");
string libcC      = Path.Combine(runtimeDir, "libc.c");
string syscallsC  = Path.Combine(runtimeDir, "syscalls.c");
string mallocC    = Path.Combine(runtimeDir, "malloc.c");
string vfsC       = Path.Combine(runtimeDir, "vfs.c");

// Build output directory
string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);
string elfPath = Path.Combine(buildDir, "doom.elf");

// ── Parse CLI args ────────────────────────────────────────────────
var opts = new SdlWindowOptions { Title = "DOOM — RV32I Emulator", GrabMouse = true };
string? customWad = null;
bool enableMExt = true; // M-extension on by default for Doom (huge speedup)

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--scale":    opts.Scale = int.Parse(args[++i]); break;
        case "--fps":      opts.TargetFps = int.Parse(args[++i]); break;
        case "--wad":      customWad = args[++i]; break;
        case "--no-grab":  opts.GrabMouse = false; break;
        case "--no-m-ext": enableMExt = false; break;
    }
}

if (customWad != null) wadPath = customWad;

if (!File.Exists(wadPath))
{
    Console.Error.WriteLine($"WAD file not found: {wadPath}");
    Console.Error.WriteLine("Place doom1.wad next to the executable or use --wad <path>");
    return 1;
}

// ── Compile C sources to RV32I ELF ───────────────────────────────
Console.WriteLine("Compiling DOOM for RV32I...");

string[] cSources = { runtimeC, softfloatC, libcC, syscallsC, mallocC, vfsC };
var objFiles = new List<string>();

// Compile each runtime source to .o
foreach (string src in cSources)
{
    if (!File.Exists(src))
    {
        Console.Error.WriteLine($"Source not found: {src}");
        return 1;
    }
    string objName = Path.GetFileNameWithoutExtension(src) + ".o";
    string objPath = Path.Combine(buildDir, objName);
    if (!Compile(src, objPath, new[] { $"-I{runtimeDir}" }, enableMExt))
        return 1;
    objFiles.Add(objPath);
}

// Compile doom_main.c (needs -I for PureDOOM.h and libc.h)
{
    string doomObj = Path.Combine(buildDir, "doom_main.o");
    if (!Compile(doomMainC, doomObj, new[] { $"-I{pureDoomH}", $"-I{runtimeDir}" }, enableMExt))
        return 1;
    objFiles.Add(doomObj);
}

// Link all objects into doom.elf
{
    string march = enableMExt ? "rv32im" : "rv32i";
    var linkArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", $"-march={march}", "-mabi=ilp32",
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

var elfInfo = new FileInfo(elfPath);
Console.WriteLine($"  doom.elf: {elfInfo.Length:N0} bytes");

// ── Build SoC ────────────────────────────────────────────────────
Console.WriteLine("Building SoC...");
byte[] elfData = File.ReadAllBytes(elfPath);
var memory  = new Memory(RamMB * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
var fb      = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
display.SetMemory(memory);
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
emu.OutputHandler = c => Console.Write(c);

// Load WAD into guest RAM
byte[] wadData = File.ReadAllBytes(wadPath);
bus.WriteByte(WadSizeAddr + 0, (byte)(wadData.Length));
bus.WriteByte(WadSizeAddr + 1, (byte)(wadData.Length >> 8));
bus.WriteByte(WadSizeAddr + 2, (byte)(wadData.Length >> 16));
bus.WriteByte(WadSizeAddr + 3, (byte)(wadData.Length >> 24));
bus.Load(WadBaseAddr, wadData, 0, wadData.Length);
Console.WriteLine($"  WAD loaded: {wadData.Length:N0} bytes at 0x{WadBaseAddr:X8}");

// ── Run ──────────────────────────────────────────────────────────
Console.WriteLine("Starting DOOM...");
var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts, midi);
return window.Run();

// ── Helper: compile a single C file to .o ────────────────────────
bool Compile(string src, string obj, string[] extraFlags, bool mExt)
{
    string name = Path.GetFileName(src);
    Console.Write($"  {name}... ");

    string march = mExt ? "rv32im" : "rv32i";
    var compileArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", $"-march={march}", "-mabi=ilp32",
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
        Console.Error.WriteLine($"FAILED (exit {proc.ExitCode})");
        Console.Error.WriteLine(stderr);
        return false;
    }
    return true;
}
