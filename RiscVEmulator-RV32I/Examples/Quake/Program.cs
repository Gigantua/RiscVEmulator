using System.Diagnostics;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

// ── Configuration ─────────────────────────────────────────────────
const uint StackPointer = 0x03FFFFF0;   // top of 64 MiB RAM
const int  RamMB        = 64;           // Quake hunk wants ≥16 MiB

string clang = @"C:\Program Files\LLVM\bin\clang.exe";

// Paths
string exeDir       = AppContext.BaseDirectory;
// Walk upward from exeDir looking for the Quake project root (the
// folder that contains Programs/ + tyrquake/). Works for both
// bin/Release/net10.0 (3 deep) and bin/x64/Release/net10.0 (4 deep).
string quakeProjDir = exeDir;
for (var d = new DirectoryInfo(exeDir); d != null; d = d.Parent)
    if (Directory.Exists(Path.Combine(d.FullName, "tyrquake"))
        && Directory.Exists(Path.Combine(d.FullName, "Programs")))
    { quakeProjDir = d.FullName; break; }
string programsDir  = Path.Combine(quakeProjDir, "Programs");
string solutionRoot = Path.GetFullPath(Path.Combine(quakeProjDir, "..", ".."));
string runtimeDir   = Path.Combine(solutionRoot, "Runtime");
string linkerLd     = Path.Combine(runtimeDir, "linker.ld");
string tyrqDir      = Path.Combine(quakeProjDir, "tyrquake");
Console.WriteLine($"  exeDir = {exeDir}");
Console.WriteLine($"  tyrqDir = {tyrqDir}");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);
string elfPath = Path.Combine(buildDir, "quake.elf");

// ── Parse CLI args ────────────────────────────────────────────────
var opts = new SdlWindowOptions { Title = "Quake — RV32I (softfloat)", GrabMouse = true, Scale = 2 };
string? pakOverride = null;
bool skipCompile = false;
bool buildOnly = false;

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--scale":        opts.Scale = int.Parse(args[++i]); break;
        case "--fps":          opts.TargetFps = int.Parse(args[++i]); break;
        case "--pak":          pakOverride = args[++i]; break;
        case "--no-grab":      opts.GrabMouse = false; break;
        case "--skip-compile": skipCompile = true; break;
        case "--build-only":   buildOnly = true; break;
    }
}

// Quake data: the shareware pak0.pak (~18.6 MiB, freely redistributable).
// Search any id1/pak0.pak under Examples/Quake/** so a single download
// covers Debug, Release, and x64 build outputs.
string? pakPath = pakOverride;
if (pakPath == null)
{
    string[] pakCandidates =
    {
        Path.Combine(exeDir, "id1", "pak0.pak"),
        Path.Combine(quakeProjDir, "id1", "pak0.pak"),
        Path.Combine(quakeProjDir, "bin", "x64", "Release", "net10.0", "id1", "pak0.pak"),
        Path.Combine(quakeProjDir, "bin", "Release", "net10.0", "id1", "pak0.pak"),
        Path.Combine(quakeProjDir, "bin", "x64", "Debug",   "net10.0", "id1", "pak0.pak"),
        Path.Combine(quakeProjDir, "bin", "Debug",   "net10.0", "id1", "pak0.pak"),
    };
    pakPath = pakCandidates.FirstOrDefault(File.Exists);
}
if (pakPath == null || !File.Exists(pakPath))
{
    Console.Error.WriteLine($"Quake data not found.");
    Console.Error.WriteLine("Place the freely-redistributable Quake shareware pak0.pak at any of:");
    Console.Error.WriteLine($"  {Path.Combine(exeDir, "id1")}{Path.DirectorySeparatorChar}pak0.pak");
    Console.Error.WriteLine($"  {Path.Combine(quakeProjDir, "id1")}{Path.DirectorySeparatorChar}pak0.pak");
    Console.Error.WriteLine("or pass --pak <path>.");
    Console.Error.WriteLine("Get it from https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz");
    return 1;
}

if (!Directory.Exists(tyrqDir))
{
    Console.Error.WriteLine($"TyrQuake source not found at {tyrqDir}");
    Console.Error.WriteLine("Clone with:  git clone https://github.com/sezero/tyrquake Examples/Quake/tyrquake");
    return 1;
}

// ── Curated TyrQuake source list (NQ + common, software renderer, no
//    network / GL / X11 / SDL / platform-specific drivers). Our own
//    Programs/{vid,snd,in,sys}_rvemu.c replace the null/X11 drivers. ─
string[] tyrqCommon = new[] {
    "alias_model.c", "buildinfo.c", "cd_null.c",
    "cl_input.c", "cmd.c", "common.c", "console.c", "crc.c", "cvar.c",
    "developer.c", "draw.c",
    "d_edge.c", "d_fill.c", "d_init.c", "d_modech.c", "d_part.c",
    "d_polyse.c", "d_scan.c", "d_sky.c", "d_sprite.c", "d_surf.c", "d_vars.c",
    "keys.c", "mathlib.c", "menu.c", "model.c", "pcx.c",
    "pr_cmds.c", "pr_edict.c", "pr_exec.c", "qpic.c", "rb_tree.c",
    "r_aclip.c", "r_alias.c", "r_bsp.c", "r_draw.c", "r_edge.c",
    "r_efrag.c", "r_lerp.c", "r_light.c", "r_main.c", "r_misc.c",
    "r_model.c", "r_part.c", "r_sky.c", "r_sprite.c", "r_surf.c", "r_vars.c",
    "screen.c", "shell.c", "snd_dma.c", "snd_mem.c", "snd_mix.c",
    "sprite_model.c", "sv_move.c", "sv_phys.c", "tga.c",
    "vid_mode.c", "wad.c", "world.c", "zone.c",
};
string[] tyrqNQ = new[] {
    "chase.c", "cl_demo.c", "cl_main.c", "cl_parse.c", "cl_tent.c",
    "host.c", "host_cmd.c",
    "net_loop.c", "net_main.c", "net_common.c", "net_none.c",
    "sbar.c", "sv_main.c", "sv_user.c", "view.c",
};
string[] rvemuShims = new[] {
    "vid_rvemu.c", "snd_rvemu.c", "in_rvemu.c", "sys_rvemu.c",
    "stubs_rvemu.c", "quake_main.c",
};
string[] runtimeC = new[] {
    "runtime.c", "softfloat.c", "libc.c", "syscalls.c",
    "malloc.c", "math.c", "vfs.c", "setjmp.c",
};

// ── Compile ──────────────────────────────────────────────────────
if (!skipCompile)
{
    Console.WriteLine("Compiling Quake for RV32I (softfloat, no extensions)...");
    var objs = new List<string>();

    string commonInc = Path.Combine(tyrqDir, "common");
    string includeInc = Path.Combine(tyrqDir, "include");
    string nqInc = Path.Combine(tyrqDir, "NQ");
    var tyrqFlags = new[] {
        $"-I{commonInc}", $"-I{includeInc}", $"-I{nqInc}", $"-I{runtimeDir}", $"-I{programsDir}",
        "-DNQ_HACK", "-DTYR_VERSION=\"0.71-rvemu\"",
        "-DTYR_VERSION_TIME=1700000000LL", "-DTYR_VERSION_NUM=0.71",
        "-D__riscv_xlen=32",
        "-include", Path.Combine(programsDir, "rvemu_config.h"),
        /* Implicit-decl + int-conversion are HARD errors here: a missing
         * prototype made `atof` default to `int atof()` and clang emitted
         * `__floatsisf` after every call, silently zeroing every entity
         * origin. The remaining -Wno flags are noise from 1996-era K&R
         * code (void* casts, missing return types on prototype lists). */
        "-Werror=implicit-function-declaration", "-Werror=int-conversion",
        "-Wno-incompatible-pointer-types",
        "-Wno-unused-result", "-Wno-deprecated-non-prototype",
        /* `-Wstringop-truncation` is a GCC warning ID; common.c has a
         * #pragma silencing it that clang doesn't recognize. Suppress the
         * meta-warning so clang stays quiet. */
        "-Wno-unknown-warning-option",
    };

    string softfpDir   = Path.Combine(runtimeDir, "softfp");
    string softfpForce = Path.Combine(softfpDir, "rvemu_softfp_force.h");
    var runtimeFlags = new[] {
        $"-I{runtimeDir}", $"-I{softfpDir}", "-include", softfpForce,
    };
    foreach (var f in runtimeC)
        if (!Compile(Path.Combine(runtimeDir, f), Path.Combine(buildDir, Path.GetFileNameWithoutExtension(f) + ".o"),
                     runtimeFlags, out string oo)) return 1; else objs.Add(oo);

    foreach (var f in rvemuShims)
        if (!Compile(Path.Combine(programsDir, f), Path.Combine(buildDir, Path.GetFileNameWithoutExtension(f) + ".o"),
                     tyrqFlags, out string oo)) return 1; else objs.Add(oo);

    foreach (var f in tyrqCommon)
        if (!Compile(Path.Combine(tyrqDir, "common", f), Path.Combine(buildDir, "tq_" + Path.GetFileNameWithoutExtension(f) + ".o"),
                     tyrqFlags, out string oo)) return 1; else objs.Add(oo);

    foreach (var f in tyrqNQ)
        if (!Compile(Path.Combine(tyrqDir, "NQ", f), Path.Combine(buildDir, "nq_" + Path.GetFileNameWithoutExtension(f) + ".o"),
                     tyrqFlags, out string oo)) return 1; else objs.Add(oo);

    // Link
    Console.Write("  Linking... ");
    var linkArgs = new List<string> {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O2", "-fno-builtin",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objs);
    linkArgs.AddRange(new[] { "-o", elfPath });
    if (!RunClang(linkArgs.ToArray())) return 1;
    Console.WriteLine("OK");
    Console.WriteLine($"  quake.elf: {new FileInfo(elfPath).Length:N0} bytes");
}

if (buildOnly)
{
    Console.WriteLine("[--build-only] exiting before SoC bring-up.");
    return 0;
}

byte[] elfData = File.ReadAllBytes(elfPath);
byte[] pakData = File.ReadAllBytes(pakPath);

// ── Build SoC ────────────────────────────────────────────────────
Console.WriteLine("Building SoC...");
var memory  = new Memory(RamMB * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
// Render resolution: 640×400 (~4× pixels of vanilla 320×200) — kept
// 4:3 aspect so the HUD/menu artwork doesn't stretch. Tied to
// BASEWIDTH/BASEHEIGHT in vid_rvemu.c.
var fb      = new FramebufferDevice(width: 640, height: 400);
var display = new DisplayControlDevice(fb);
display.SetMemory(memory);
var kbd       = new KeyboardDevice();
var mouse     = new MouseDevice();
var rtc       = new RealTimeClockDevice();
var audioBuf  = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();
var disk      = new DiskDevice();
bus.RegisterPeripheral(uart);  bus.RegisterPeripheral(fb);
bus.RegisterPeripheral(display); bus.RegisterPeripheral(kbd);
bus.RegisterPeripheral(mouse); bus.RegisterPeripheral(rtc);
bus.RegisterPeripheral(audioBuf); bus.RegisterPeripheral(audioCtrl);
bus.RegisterPeripheral(disk);

uart.OutputHandler = c => Console.Write(c);
disk.LoadFile(pakData, bus);
Console.WriteLine($"  pak0.pak on disk: {pakData.Length:N0} bytes");

var regs = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
// Hook softfloat ABI entry points so the JIT short-circuits each
// libgcc call with inline x86 SSE (~50x faster per op on Quake's hot
// renderer path).
var sfSyms = ElfSymbols.ReadFunctionSymbols(elfData);
// Set RVEMU_SF_HOOKS=0 to disable the SSE softfloat shortcut and run
// every float op through Bellard's softfp interpreter path. Useful for
// bisecting suspected hook-vs-softfp divergence.
// SSE softfloat hook: each __addsf3/__mulsf3/etc. entry point gets
// short-circuited to a 4-instruction x86 SSE sequence instead of
// running the ~30-instruction Bellard softfp body. ~18x on softfloat
// microbench, ~10% on Quake's full frame. (Earlier scare-disabled
// during physics debugging — actual culprits were TF stubs + atof.)
// Set RVEMU_SF_HOOKS=0 to disable.
bool sfHooksEnabled = Environment.GetEnvironmentVariable("RVEMU_SF_HOOKS") != "0";
int sfHooks = sfHooksEnabled ? Emulator.InstallSoftFloatHooks(sfSyms) : 0;
Console.WriteLine($"  softfloat SSE hooks: {sfHooks}{(sfHooksEnabled ? "" : " (disabled)")}");
regs.Write(2, StackPointer);

var emu = new Emulator(bus, regs, entry);
emu.OutputHandler = c => Console.Write(c);

Console.WriteLine("Starting Quake...");
var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts);
return window.Run();

// ── Build helper ────────────────────────────────────────────────
bool Compile(string src, string obj, string[] extraFlags, out string outObj)
{
    outObj = obj;
    if (!File.Exists(src)) {
        Console.Error.WriteLine($"  missing: {src}");
        return false;
    }
    if (File.Exists(obj) && File.GetLastWriteTime(obj) > File.GetLastWriteTime(src)) {
        return true;   // up to date
    }
    string name = Path.GetFileName(src);
    Console.Write($"  {name,-22}... ");
    var argList = new List<string> {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-O3",
        "-fno-builtin", "-fsigned-char", "-fno-strict-aliasing",
        "-Wno-unused-command-line-argument",
        "-c", src, "-o", obj,
    };
    argList.AddRange(extraFlags);
    var (ok, errStr) = RunClangCapture(argList.ToArray());
    if (ok)
    {
        int n = CountWarnings(errStr);
        if (n == 0) { Console.WriteLine("OK"); return true; }
        // Print "OK (N warnings)" and a one-line summary per warning,
        // indented so the file's line stays the anchor. Set
        // RVEMU_QUAKE_VERBOSE=1 to print the full clang stderr.
        Console.WriteLine($"OK ({n} warning{(n == 1 ? "" : "s")})");
        if (Environment.GetEnvironmentVariable("RVEMU_QUAKE_VERBOSE") == "1")
        {
            foreach (var line in errStr.Split('\n'))
                if (line.Length > 0) Console.Error.WriteLine("    " + line);
        }
        else
        {
            foreach (var line in errStr.Split('\n'))
                if (line.Contains(": warning:"))
                    Console.Error.WriteLine("    " + line.Trim());
            Console.Error.WriteLine("    (set RVEMU_QUAKE_VERBOSE=1 for full clang output)");
        }
        return true;
    }
    Console.WriteLine("FAIL");
    if (errStr.Length > 0) Console.Error.Write(errStr);
    return false;
}

static int CountWarnings(string err)
{
    int n = 0;
    foreach (var line in err.Split('\n'))
        if (line.Contains(": warning:")) n++;
    return n;
}

(bool ok, string err) RunClangCapture(string[] argv)
{
    var psi = new ProcessStartInfo(clang) { UseShellExecute = false,
        RedirectStandardOutput = true, RedirectStandardError = true };
    foreach (var a in argv) psi.ArgumentList.Add(a);
    var p = Process.Start(psi)!;
    string outStr = p.StandardOutput.ReadToEnd();
    string errStr = p.StandardError.ReadToEnd();
    p.WaitForExit();
    return (p.ExitCode == 0, errStr + outStr);
}

bool RunClang(string[] argv)
{
    var (ok, err) = RunClangCapture(argv);
    if (!ok && err.Length > 0) Console.Error.Write(err);
    return ok;
}
