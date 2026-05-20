#:property AllowUnsafeBlocks=true

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

const uint MEM_SIZE = 1u << 20;     // 1 MiB
const uint OUT_BASE = 0x80000u;
const uint OUT_END  = 0xC0000u;

var root     = Path.GetFullPath(Path.GetDirectoryName(GetThisFile())!);
var emuDir   = Path.Combine(root, "emu");
var guestDir = Path.Combine(root, "guest");
var buildDir = Path.Combine(root, "build");
Directory.CreateDirectory(buildDir);

var dllPath = Path.Combine(buildDir, "rv32emu.dll");
var elfPath = Path.Combine(buildDir, "guest.elf");
var binPath = Path.Combine(buildDir, "guest.bin");

Console.WriteLine("=== Step 1: build the emulator (clang++ -> DLL) ===");
Run("clang++", "-O2", "-std=c++20", "-shared",
    "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
    Path.Combine(emuDir, "core.cpp"),
    "-o", dllPath);

Console.WriteLine("\n=== Step 2: build the guest (clang -> RV32I ELF) ===");
Run("clang",
    "--target=riscv32-unknown-elf",
    "-march=rv32i", "-mabi=ilp32",
    "-nostdlib", "-ffreestanding", "-fno-builtin",
    "-O2", "-fno-pic",
    "-fuse-ld=lld",
    "-Wl,-T," + Path.Combine(guestDir, "link.ld"),
    "-Wl,--no-relax",
    Path.Combine(guestDir, "start.S"),
    Path.Combine(guestDir, "guest.c"),
    "-o", elfPath);

Console.WriteLine("\n=== Step 3: extract flat binary (llvm-objcopy) ===");
Run("llvm-objcopy", "-O", "binary", elfPath, binPath);

var image = File.ReadAllBytes(binPath);
Console.WriteLine($"Guest binary: {image.Length} bytes\n");

Console.WriteLine("=== Step 4: allocate 1 MiB, copy guest in, run on emulator ===");
var mem = new byte[MEM_SIZE];
Array.Copy(image, 0, mem, 0, image.Length);

// Load the DLL we just built and resolve DllImport against it.
var loaded = NativeLibrary.Load(dllPath);
NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, asm, search) =>
    name == "rv32emu" ? loaded : IntPtr.Zero);

var sw = Stopwatch.StartNew();
unsafe {
    fixed (byte* p = mem) {
        IntPtr core = Native.core_create((IntPtr)p, 0);
        Native.core_run(core);
        sw.Stop();
        Console.WriteLine($"Emulator halted (EBREAK). PC = 0x{Native.core_get_pc(core):x8} after {sw.Elapsed.TotalMilliseconds:F1} ms");
        Native.core_destroy(core);
    }
}

Console.WriteLine("\n=== Step 5: guest output (read from 0x80000) ===");
Console.WriteLine("------------------------------------------------");
var sb = new StringBuilder();
for (uint i = OUT_BASE; i < OUT_END && i < mem.Length; i++) {
    byte b = mem[i];
    if (b == 0) break;
    sb.Append((char)b);
}
Console.Write(sb.ToString());
Console.WriteLine("------------------------------------------------");

Console.WriteLine("\n=== Step 6: host-side CRC32 verification ===");
var hostCrc = Crc32("Hello, RV32I!");
Console.WriteLine($"Host  CRC32(\"Hello, RV32I!\") = 0x{hostCrc:x8}");

var output = sb.ToString();
const string MARK = "crc32(\"Hello, RV32I!\") = 0x";
int idx = output.IndexOf(MARK, StringComparison.Ordinal);
if (idx >= 0) {
    var hex = output.Substring(idx + MARK.Length, 8);
    uint guestCrc = Convert.ToUInt32(hex, 16);
    Console.WriteLine($"Guest CRC32                  = 0x{guestCrc:x8}");
    Console.WriteLine(guestCrc == hostCrc ? "CRC32 MATCH" : "CRC32 MISMATCH");
} else {
    Console.WriteLine("(could not find guest CRC line in output)");
}

return 0;

static void Run(string exe, params string[] args)
{
    var psi = new ProcessStartInfo {
        FileName = exe,
        RedirectStandardOutput = true,
        RedirectStandardError  = true,
        UseShellExecute = false,
        CreateNoWindow  = true,
    };
    foreach (var a in args) psi.ArgumentList.Add(a);
    Console.WriteLine("> " + exe + " " + string.Join(" ", args));
    using var p = Process.Start(psi)!;
    string stdout = p.StandardOutput.ReadToEnd();
    string stderr = p.StandardError.ReadToEnd();
    p.WaitForExit();
    if (stdout.Length > 0) Console.WriteLine(stdout);
    if (stderr.Length > 0) Console.Error.WriteLine(stderr);
    if (p.ExitCode != 0) throw new Exception($"{exe} exited with code {p.ExitCode}");
}

static uint Crc32(string s)
{
    uint crc = 0xFFFFFFFFu;
    foreach (var ch in s) {
        crc ^= (byte)ch;
        for (int i = 0; i < 8; i++) {
            if ((crc & 1u) != 0) crc = (crc >> 1) ^ 0xEDB88320u;
            else                  crc >>= 1;
        }
    }
    return ~crc;
}

static string GetThisFile([System.Runtime.CompilerServices.CallerFilePath] string p = "") => p;

static class Native
{
    const string Dll = "rv32emu";
    [DllImport(Dll)] public static extern IntPtr core_create(IntPtr mem, uint entryPc);
    [DllImport(Dll)] public static extern bool   core_step(IntPtr core);
    [DllImport(Dll)] public static extern void   core_run(IntPtr core);
    [DllImport(Dll)] public static extern void   core_destroy(IntPtr core);
    [DllImport(Dll)] public static extern uint   core_get_pc(IntPtr core);
    [DllImport(Dll)] public static extern uint   core_get_reg(IntPtr core, int idx);
}
