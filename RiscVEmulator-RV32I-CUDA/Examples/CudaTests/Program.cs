using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Cuda;

// ── CudaTests ─────────────────────────────────────────────────────────
// Console text IO on the GPU core, two modes:
//
//   [--jit]          Interactive echo. The guest is plain C against the
//                    bare-metal runtime (printf → UART ring, fgets → keyboard
//                    mailbox); the host bridges the real console — stdout is
//                    the drained UART stream, and every line you type is fed
//                    into the keyboard FIFO (one key staged per launch).
//                    Empty line or "exit" ends the guest. --jit runs it on
//                    the rvcud translated engine (uop stream + exec_block
//                    PTX JIT) instead of the per-instruction interpreter.
//
//   --tests [dir] [--interp]
//                    C-library test suite: every c-testsuite single-exec
//                    program is compiled against the SAME runtime libc and
//                    run to completion on its own CUDA core, all cores in
//                    parallel; stdout + exit code are compared against the
//                    .expected files. Tests the parallel interpreter phase
//                    doesn't finish quickly continue on the JIT engine
//                    (one at a time — the translated code image is global).
//                    --interp is the debug profile: pure interpreter, no JIT.
//                    Default dir: Examples/CudaTests/c-testsuite/single-exec
//                    (vendored; 00040.c — the ~390M-instruction 8-queens
//                    solver — is deliberately excluded as too long-running).

const string Lib = "rv32i_cuda";
[DllImport(Lib)] static extern int  cuda_rv32i_init(int n, uint memBytes);
[DllImport(Lib)] static extern int  cuda_rv32i_write_mem(int core, byte[] src, uint off, uint len);
[DllImport(Lib)] static extern int  cuda_rv32i_read_mem(int core, byte[] dst, uint off, uint len);
[DllImport(Lib)] static extern void cuda_rv32i_set_reg(int core, int i, uint v);
[DllImport(Lib)] static extern void cuda_rv32i_set_entry(int core, uint pc);
[DllImport(Lib)] static extern int  cuda_rv32i_step_all(int budget);
[DllImport(Lib)] static extern uint cuda_rv32i_get_pc(int core);
[DllImport(Lib)] static extern void cuda_rv32i_shutdown();
[DllImport(Lib)] static extern int  cuda_rvcud_set_code(byte[] src, uint len, uint baseAddr, uint entry);
[DllImport(Lib)] static extern int  cuda_rvcud_step_all(int budget);

const uint Sp = 0x00EFFF00;
const int  RamMB = 16;
string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
    root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("solution root not found"); return 2; }

string runtimeDir = Path.Combine(root, "Runtime");
string programsDir = Path.Combine(root, "Examples", "CudaTests", "Programs");
string buildDir = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);

if (args.Length > 0 && args[0] == "--tests")
    return RunTests(
        args.Skip(1).FirstOrDefault(a => !a.StartsWith("--"))
            ?? Path.Combine(root, "Examples", "CudaTests", "c-testsuite", "single-exec"),
        args.Contains("--interp"));

// ── Interactive echo mode ─────────────────────────────────────────────
string elf = Path.Combine(buildDir, "echo_guest.elf");

Console.WriteLine("Compiling echo_guest.c + runtime for RV32I...");
string[] sources =
{
    Path.Combine(runtimeDir, "crt0.c"),
    Path.Combine(runtimeDir, "runtime.c"),
    Path.Combine(runtimeDir, "libc.c"),
    Path.Combine(runtimeDir, "stdio_file.c"),    // fgets (stdin + FILE streams)
    Path.Combine(runtimeDir, "syscalls.c"),
    Path.Combine(runtimeDir, "softfloat.c"),     // printf's %f path uses doubles
    Path.Combine(programsDir, "echo_guest.c"),
};
var link = new List<string> {
    "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
    "-nostdlib", "-nostartfiles", "-fuse-ld=lld",
    $"-Wl,-T,{Path.Combine(runtimeDir, "linker.ld")}" };
foreach (string s in sources)
{
    string obj = Path.Combine(buildDir, Path.GetFileNameWithoutExtension(s) + ".o");
    if (!Clang(new[] {
            "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
            "-nostdlib", "-O2", "-fno-builtin", "-ffreestanding",
            $"-I{runtimeDir}", "-c", s, "-o", obj }))
        return 2;
    link.Add(obj);
}
link.Add("-o"); link.Add(elf);
if (!Clang(link.ToArray())) return 2;

bool jit = args.Contains("--jit");
Console.WriteLine($"Running on the GPU core ({(jit ? "rvcud JIT" : "interpreter")})...\n");
using var emu = new CudaEmulator(RamMB * 1024 * 1024);
emu.UseRvcud = jit;                              // must be set before CommitImage

// Guest output goes straight to the console; a short tail of it tells the
// host when the guest printed its prompt and is waiting for the next line.
string tail = "";
emu.OutputHandler = c => { Console.Write(c); tail = (tail.Length >= 8 ? tail[1..] : tail) + c; };

uint entry = emu.LoadElf(File.ReadAllBytes(elf));
emu.CommitImage();
emu.SetReg(2, Sp);
emu.SetEntry(entry);

bool StepToPrompt()
{
    for (int i = 0; i < 100_000 && !emu.IsHalted; i++)
    {
        if (tail.EndsWith("echo> ")) return true;
        emu.StepN(100_000);
    }
    return false;
}

while (StepToPrompt())
{
    string line = Console.ReadLine() ?? "";      // EOF → empty line → guest exits
    tail = "";
    foreach (char ch in line) emu.Keyboard.EnqueueKey((byte)ch, true);
    emu.Keyboard.EnqueueKey((byte)'\n', true);
}
if (!emu.IsHalted) { Console.Error.WriteLine("\nFAIL: guest neither prompted nor halted"); return 2; }

Console.WriteLine($"guest exited (code {emu.ExitCode})");
return emu.ExitCode;

// ── C-library test-suite mode ─────────────────────────────────────────
int RunTests(string suiteDir, bool interpOnly)
{
    const uint TestRam = 0x00200000;   // 2 MiB flat buffer per core
    const uint TestSp  = 0x001FFF00;
    const uint IoBase  = 0x001E0000;   // must match test_syscalls.c
    const uint IoCap   = 0xFF00;

    if (!Directory.Exists(suiteDir)) { Console.Error.WriteLine($"test suite not found: {suiteDir}"); return 2; }
    string[] tests = Directory.GetFiles(suiteDir, "*.c").OrderBy(p => p).ToArray();
    if (tests.Length == 0) { Console.Error.WriteLine($"no .c tests in {suiteDir}"); return 2; }
    Console.WriteLine($"[cudatests] {tests.Length} c-testsuite guests, one per CUDA core (rv32im + Runtime libc)");

    // The runtime, compiled once. test_syscalls.c replaces syscalls.c (small
    // in-RAM IO block instead of the sparse 1 GiB device map, so hundreds of
    // cores fit) and brings its own _start, so no crt0.c either.
    string testBuild = Path.Combine(buildDir, "tests"); Directory.CreateDirectory(testBuild);
    string[] cflags = { "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                        "-nostdlib", "-O2", "-fno-builtin", "-ffreestanding", $"-I{runtimeDir}" };
    var rtObjs = new List<string>();
    foreach (string s in new[] { "runtime.c", "libc.c", "stdio_file.c", "malloc.c", "softfloat.c", "math.c" }
                 .Select(f => Path.Combine(runtimeDir, f))
                 .Append(Path.Combine(programsDir, "test_syscalls.c")))
    {
        string obj = Path.Combine(testBuild, Path.GetFileNameWithoutExtension(s) + ".o");
        var (ok, err) = ClangQ(cflags.Append("-c").Append(s).Append("-o").Append(obj).ToArray());
        if (!ok) { Console.Error.WriteLine(err); return 2; }
        rtObjs.Add(obj);
    }

    // Compile + link every test in parallel; failures become SKIP. The tests
    // themselves are built WITHOUT -ffreestanding: c-testsuite assumes a hosted
    // environment, where falling off the end of main() returns 0 (freestanding
    // main is not special and would "return" whatever the last call left in a0).
    Console.WriteLine("compiling guests...");
    string[] tcflags = cflags.Where(f => f != "-ffreestanding").ToArray();
    var elves = new string?[tests.Length];
    var skip  = new string?[tests.Length];
    Parallel.For(0, tests.Length, i =>
    {
        string name = Path.GetFileNameWithoutExtension(tests[i]);
        string obj = Path.Combine(testBuild, name + ".o");
        string elfPath = Path.Combine(testBuild, name + ".elf");
        var (ok, err) = ClangQ(tcflags.Append("-c").Append(tests[i]).Append("-o").Append(obj).ToArray());
        if (ok)
            (ok, err) = ClangQ(new[] { "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                    "-nostdlib", "-nostartfiles", "-fuse-ld=lld",
                    $"-Wl,-T,{Path.Combine(runtimeDir, "linker.ld")}" }
                .Concat(rtObjs).Append(obj).Append("-o").Append(elfPath).ToArray());
        if (ok) elves[i] = elfPath;
        else skip[i] = err.Split('\n').FirstOrDefault(l => l.Contains("error")) ?? "compile failed";
    });
    int[] built = Enumerable.Range(0, tests.Length).Where(i => elves[i] != null).ToArray();
    Console.WriteLine($"  {built.Length} built, {tests.Length - built.Length} skipped (don't compile/link against our libc)");
    if (built.Length == 0) return 2;

    // One core per test: load each ELF into its core's RAM and arm sp/pc.
    if (cuda_rv32i_init(built.Length, TestRam) != 0) { Console.Error.WriteLine("cuda_rv32i_init failed"); return 2; }
    for (int c = 0; c < built.Length; c++)
    {
        var image = new byte[TestRam];
        uint e = ElfLoader.Load(File.ReadAllBytes(elves[built[c]]!), new ArrayBus(image));
        int hi = (int)IoBase; while (hi > 0 && image[hi - 1] == 0) hi--;   // loaded extent; device RAM is pre-zeroed
        cuda_rv32i_write_mem(c, image, 0, (uint)((hi + 0xFFF) & ~0xFFF));
        cuda_rv32i_set_reg(c, 2, TestSp);
        cuda_rv32i_set_entry(c, e);
    }

    // Phase 1 — every core in parallel on the interpreter until all halted
    // (_exit ebreaks; a trap also halts). The overview line is the wait-for-
    // all-cores progress and names the stragglers once only a few remain.
    // The phase ends when several batches pass without ANY core finishing:
    // quickly when the JIT phase will take over, or after 600M instructions
    // of grace in --interp mode (anything beyond that is declared hung; the
    // vendored suite excludes 00040.c, whose 8-queens solver alone ran ~390M).
    var sw = Stopwatch.StartNew();
    int staleMax = interpOnly ? 300 : 10;
    int halted = 0, stale = 0;
    while (halted < built.Length && stale < staleMax)
    {
        int rc = cuda_rv32i_step_all(2_000_000);
        if (rc != 0) { Console.Error.WriteLine($"step_all failed (CUDA error {rc})"); return 2; }
        int now = 0;
        var running = new List<string>();
        for (int c = 0; c < built.Length; c++)
            if ((cuda_rv32i_get_pc(c) & 0x80000000u) != 0) now++;
            else if (running.Count < 4) running.Add(Path.GetFileName(tests[built[c]]));
        stale = now == halted ? stale + 1 : 0;
        halted = now;
        string lag = now < built.Length && built.Length - now <= 4 ? $" — waiting on {string.Join(", ", running)}" : "";
        Console.Write($"\r  running... {halted}/{built.Length} cores done ({sw.Elapsed.TotalSeconds:F1} s){lag}   ");
    }
    Console.WriteLine();

    // Collect per-core results now — phase 2 re-inits the emulator.
    // (state: halted?, pc, done flag, exit code, stdout, hung-after-jit?)
    (uint done, uint code, string output) ReadIo(int core)
    {
        var hdr = new byte[16];
        cuda_rv32i_read_mem(core, hdr, IoBase, 16);
        uint d = BitConverter.ToUInt32(hdr, 0), x = BitConverter.ToUInt32(hdr, 4);
        int n = (int)Math.Min(BitConverter.ToUInt32(hdr, 8), IoCap);
        var buf = new byte[(n + 3) & ~3];                // read_mem needs word-aligned lengths
        if (n > 0) cuda_rv32i_read_mem(core, buf, IoBase + 0x10, (uint)buf.Length);
        return (d, x, Encoding.ASCII.GetString(buf, 0, n));
    }
    var res = new (bool halted, uint pc, uint done, uint code, string output, bool hung)[built.Length];
    for (int c = 0; c < built.Length; c++)
    {
        uint pc = cuda_rv32i_get_pc(c);
        var (d, x, o) = ReadIo(c);
        res[c] = ((pc & 0x80000000u) != 0, pc & 0x7FFFFFFFu, d, x, o, false);
    }
    cuda_rv32i_shutdown();

    // Phase 2 — JIT: any test the interpreter grace didn't finish reruns from
    // scratch on the rvcud translated engine (uop stream + exec_block PTX
    // JIT). The rvcud driver is single-core-lead by design (exec dispatch,
    // budget and halt tracking all follow core 0), so each straggler gets a
    // fresh 1-core instance — which is also why this phase is sequential.
    if (!interpOnly)
        for (int c = 0; c < built.Length; c++)
        {
            if (res[c].halted) continue;
            string name = Path.GetFileName(tests[built[c]]);
            Console.Write($"  jit: rerunning {name} on the translated engine...");
            byte[] elfBytes = File.ReadAllBytes(elves[built[c]]!);
            var image = new byte[TestRam];
            uint e2 = ElfLoader.Load(elfBytes, new ArrayBus(image));
            var (_, codeHi) = ElfLoader.ReadOnlyCodeSpan(elfBytes);
            if (cuda_rv32i_init(1, TestRam) != 0) { Console.WriteLine(" init failed"); continue; }
            cuda_rv32i_write_mem(0, image, 0, TestRam);
            cuda_rv32i_set_reg(0, 2, TestSp);
            cuda_rv32i_set_entry(0, e2);
            if (cuda_rvcud_set_code(image, codeHi != 0 ? codeHi : TestRam, 0, e2) != 0)
            { Console.WriteLine(" set_code failed"); cuda_rv32i_shutdown(); continue; }
            for (int b = 0; b < 12 && (cuda_rv32i_get_pc(0) & 0x80000000u) == 0; b++)
                cuda_rvcud_step_all(50_000_000);
            uint pc = cuda_rv32i_get_pc(0);
            var (d, x, o) = ReadIo(0);
            res[c] = ((pc & 0x80000000u) != 0, pc & 0x7FFFFFFFu, d, x, o, (pc & 0x80000000u) == 0);
            cuda_rv32i_shutdown();
            Console.WriteLine(res[c].hung ? " still running after 600M instructions — hung"
                                          : $" done ({sw.Elapsed.TotalSeconds:F1} s)");
        }

    // Verdicts: done-flag + exit code + stdout vs the .expected file.
    int pass = 0; var bad = new List<string>();
    for (int c = 0; c < built.Length; c++)
    {
        int i = built[c];
        string name = Path.GetFileName(tests[i]);
        var r = res[c];
        if (r.done == 0)
        {
            bad.Add($"{name}: {(r.halted && !r.hung ? $"CRASH (trap, pc=0x{r.pc:X8})" : "HANG (budget exhausted)")}");
            continue;
        }
        string expFile = tests[i] + ".expected";
        string expected = File.Exists(expFile) ? File.ReadAllText(expFile).Replace("\r\n", "\n") : "";
        if (r.code == 0 && r.output == expected) { pass++; continue; }
        File.WriteAllText(Path.Combine(testBuild, name + ".out"), r.output);   // for diffing vs .expected
        bad.Add($"{name}: FAIL ({(r.code != 0 ? $"exit {(int)r.code}" : $"output mismatch, got {r.output.Length} B want {expected.Length} B")})");
    }

    Console.WriteLine($"\n  PASS {pass}/{built.Length} run, {bad.Count} bad, {tests.Length - built.Length} skipped — {sw.Elapsed.TotalSeconds:F1} s on GPU");
    foreach (string b in bad) Console.WriteLine($"  {b}");
    if (built.Length < tests.Length)
    {
        Console.WriteLine("  skipped:");
        for (int i = 0; i < tests.Length; i++)
            if (skip[i] != null)
                Console.WriteLine($"    {Path.GetFileName(tests[i])}: {skip[i]![..Math.Min(skip[i]!.Length, 120)]}");
    }
    return bad.Count == 0 ? 0 : 1;
}

bool Clang(string[] a)
{
    var (ok, err) = ClangQ(a);
    if (!ok) Console.Error.WriteLine($"clang failed\n{err}");
    return ok;
}

(bool ok, string err) ClangQ(string[] a)
{
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var x in a) psi.ArgumentList.Add(x);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    return (p.ExitCode == 0, err);
}

sealed class ArrayBus : IMemoryBus
{
    private readonly byte[] _r;
    public ArrayBus(byte[] r) => _r = r;
    public int RamSize => _r.Length;
    public IReadOnlyList<IPeripheral> Peripherals => Array.Empty<IPeripheral>();
    public byte ReadByte(uint a) => _r[a];
    public ushort ReadHalfWord(uint a) => (ushort)(_r[a] | (_r[a+1] << 8));
    public uint ReadWord(uint a) => (uint)(_r[a] | (_r[a+1]<<8) | (_r[a+2]<<16) | (_r[a+3]<<24));
    public void WriteByte(uint a, byte v) => _r[a] = v;
    public void WriteHalfWord(uint a, ushort v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); }
    public void WriteWord(uint a, uint v) { _r[a]=(byte)v; _r[a+1]=(byte)(v>>8); _r[a+2]=(byte)(v>>16); _r[a+3]=(byte)(v>>24); }
    public void Load(uint address, byte[] s, int o, int len) => Array.Copy(s, o, _r, (int)address, len);
}
