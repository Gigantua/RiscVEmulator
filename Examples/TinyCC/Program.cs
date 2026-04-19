using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

// ── JIT source: written into emulator RAM at JIT_SRC_ADDR ─────────────
// The RV32I program reads it from this fixed address, prints it, then
// hands it to TinyCC. Keeping it here makes it easy to edit without
// recompiling the ~400 KB TinyCC ELF.
const uint JitSrcAddress = 0x00D00000;
const string JitSrc =
@"extern int printf(const char *fmt, ...);
extern int putchar(int c);

int fib(int n) {
    return n <= 1 ? n : fib(n-1) + fib(n-2);
}

int count_primes(int limit) {
    char sieve[1024];
    int i, j, count = 0;
    for (i = 0; i <= limit; i++) sieve[i] = 1;
    sieve[0] = sieve[1] = 0;
    for (i = 2; i * i <= limit; i++)
        if (sieve[i])
            for (j = i*i; j <= limit; j += i)
                sieve[j] = 0;
    for (i = 2; i <= limit; i++)
        if (sieve[i]) count++;
    return count;
}

void mandelbrot(int w, int h) {
    const char *pal = "" .`-,:;!|=+xX$#@*"";
    int x, y, n;
    for (y = 0; y < h; y++) {
        int ci = ((h - 1 - 2*y) * 256) / h;
        for (x = 0; x < w; x++) {
            int cr = (x * 896) / w - 512;
            int zr = 0, zi = 0;
            for (n = 0; n < 32; n++) {
                if (zr*zr + zi*zi >= 262144) break;
                int t = ((zr*zr - zi*zi) >> 8) + cr;
                zi  = ((2*zr*zi)       >> 8) + ci;
                zr  = t;
            }
            putchar(pal[n >= 32 ? 16 : n / 2]);
        }
        putchar('\n');
    }
}
";

Console.WriteLine("JIT source (will be written to emulator RAM and compiled at runtime):");
Console.WriteLine(new string('─', 41));
Console.Write(JitSrc);
Console.WriteLine(new string('─', 41));
Console.WriteLine();

// ── Configuration ─────────────────────────────────────────────────
const uint StackPointer = 0x00EFFF00;
const int  RamMB        = 16;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";

string exeDir = AppContext.BaseDirectory;

// Walk up from the output dir until we find the solution root
string? solutionRoot = exeDir;
while (solutionRoot != null && !File.Exists(Path.Combine(solutionRoot, "RiscVEmulator.sln")))
    solutionRoot = Path.GetDirectoryName(solutionRoot);
if (solutionRoot == null)
    throw new DirectoryNotFoundException($"Cannot find RiscVEmulator.sln starting from {exeDir}");

string runtimeDir  = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Runtime");
string testProgDir = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Programs");
string linkerLd    = Path.Combine(testProgDir, "linker.ld");
string tinyccDir   = Path.Combine(solutionRoot, "Examples", "TinyCC", "tinycc");
string includeDir  = Path.Combine(solutionRoot, "Examples", "TinyCC", "include");
string programsDir = Path.Combine(solutionRoot, "Examples", "TinyCC", "Programs");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);
string elfPath = Path.Combine(buildDir, "tinycc_demo.elf");

Console.WriteLine("TinyCC JIT Demo — RV32I emulator");
Console.WriteLine($"Compiling TinyCC + demo for RV32I+M...");

// ── Runtime sources (compiled separately with -O3) ─────────────────
// tinycc_demo.c is compiled separately (with -O1) because it includes
// all of TinyCC via ONE_SOURCE=1 and clang -O3 on ~15k lines is slow.
string[] runtimeSources =
{
    Path.Combine(runtimeDir, "runtime.c"),
    Path.Combine(runtimeDir, "libc.c"),
    Path.Combine(runtimeDir, "malloc.c"),
    Path.Combine(runtimeDir, "syscalls.c"),
    Path.Combine(runtimeDir, "softfloat.c"),
    Path.Combine(runtimeDir, "setjmp.c"),
    Path.Combine(programsDir, "stubs.c"),
};

string demoSource = Path.Combine(programsDir, "tinycc_demo.c");

var objFiles = new List<string>();

// Compile runtime files with -O3
foreach (string src in runtimeSources)
{
    if (!File.Exists(src)) { Console.Error.WriteLine($"Source not found: {src}"); return 1; }
    string obj = Path.Combine(buildDir, Path.GetFileNameWithoutExtension(src) + ".o");
    Console.Write($"  {Path.GetFileName(src)} (O3)... ");
    string[] extra = { $"-I{includeDir}", $"-I{runtimeDir}" };
    if (!CompileO3(src, obj, extra)) return 1;
    Console.WriteLine("OK");
    objFiles.Add(obj);
}

// Compile tinycc_demo.c with -O1 (TCC source is large; -O3 is very slow)
{
    if (!File.Exists(demoSource)) { Console.Error.WriteLine($"Source not found: {demoSource}"); return 1; }
    string obj = Path.Combine(buildDir, "tinycc_demo.o");
    Console.Write($"  {Path.GetFileName(demoSource)} (O1)... ");
    // Include order: stubs first, then runtime headers, then tinycc sources
    string[] extra = {
        "-DONE_SOURCE=1", "-DTCC_TARGET_RISCV32=1",
        $"-I{includeDir}", $"-I{runtimeDir}", $"-I{tinyccDir}"
    };
    if (!CompileO1(demoSource, obj, extra)) return 1;
    Console.WriteLine("OK");
    objFiles.Add(obj);
}

// ── Link ───────────────────────────────────────────────────────────
{
    var linkArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O1", "-fno-builtin",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objFiles);
    linkArgs.Add("-o");
    linkArgs.Add(elfPath);

    Console.Write("  Linking... ");
    if (!RunClang(linkArgs.ToArray())) return 1;
    Console.WriteLine("OK");
}

Console.WriteLine($"  tinycc_demo.elf: {new FileInfo(elfPath).Length:N0} bytes");

// ── Run headlessly ─────────────────────────────────────────────────
Console.WriteLine("\nRunning inside RV32I emulator...");
Console.WriteLine(new string('─', 40));

byte[] elfData = File.ReadAllBytes(elfPath);
var memory  = new Memory(RamMB * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();

bus.RegisterPeripheral(uart);

var output = new System.Text.StringBuilder();
uart.OutputHandler = c =>
{
    Console.Write(c);
    output.Append(c);
};

var regs   = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, StackPointer);

// Write JIT source into emulator RAM so the running program can read it
byte[] jitSrcBytes = System.Text.Encoding.ASCII.GetBytes(JitSrc);
memory.Load(JitSrcAddress, jitSrcBytes, 0, jitSrcBytes.Length);
// null terminator is already zero (Memory is zero-initialised)

var emu = new Emulator(bus, regs, entry);
emu.EnableMExtension = true;  // TCC emits MUL/DIV instructions
emu.OutputHandler = c =>
{
    Console.Write(c);
    output.Append(c);
};

// Run until halt (exit() syscall or EBREAK)
const long MaxSteps = 500_000_000L;
long steps = 0;
while (!emu.IsHalted && steps < MaxSteps)
{
    emu.StepN(500_000);
    steps += 500_000;
}

Console.WriteLine(new string('─', 40));
if (!emu.IsHalted)
{
    Console.Error.WriteLine($"ERROR: emulator did not halt within {MaxSteps:N0} steps");
    return 1;
}
Console.WriteLine($"\nExit code: {emu.ExitCode}");
return emu.ExitCode;

// ── Helpers ────────────────────────────────────────────────────────
bool CompileO3(string src, string obj, string[] extraFlags)
{
    var args = new List<string>
    {
        "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char", "-c",
    };
    args.AddRange(extraFlags);
    args.Add(src); args.Add("-o"); args.Add(obj);
    return RunClang(args.ToArray());
}

bool CompileO1(string src, string obj, string[] extraFlags)
{
    var args = new List<string>
    {
        "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O1", "-fno-builtin", "-fsigned-char", "-c",
        "-Wno-everything",  // TinyCC source has many warnings under clang
    };
    args.AddRange(extraFlags);
    args.Add(src); args.Add("-o"); args.Add(obj);
    return RunClang(args.ToArray());
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
