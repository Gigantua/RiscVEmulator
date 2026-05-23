#:property AllowUnsafeBlocks=true

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

const uint MEM_SIZE = 1u << 20;     // 1 MiB
const uint OUT_BASE = 0x80000u;
const uint OUT_END  = 0xC0000u;

var root     = Path.GetFullPath(Path.GetDirectoryName(GetThisFile())!);
var emuDir   = Path.Combine(root, "emu");
var guestDir = Path.Combine(root, "guest");
var buildDir = Path.Combine(root, "build");
Directory.CreateDirectory(buildDir);

var elfPath = Path.Combine(buildDir, "guest.elf");
var binPath = Path.Combine(buildDir, "guest.bin");

var cores = new (string Name, string Src, string Dll)[] {
    ("rv32i", Path.Combine(emuDir, "core_rv32i.cpp"), Path.Combine(buildDir, "rv32emu_rv32i.dll")),
    ("6op",  Path.Combine(emuDir, "core_6op.cpp"),  Path.Combine(buildDir, "rv32emu_6op.dll")),
    ("5op",  Path.Combine(emuDir, "core_5op.cpp"),  Path.Combine(buildDir, "rv32emu_5op.dll")),
    ("4op",  Path.Combine(emuDir, "core_4op.cpp"),  Path.Combine(buildDir, "rv32emu_4op.dll")),
    ("1op",  Path.Combine(emuDir, "core_1op.cpp"),  Path.Combine(buildDir, "rv32emu_1op.dll")),
    ("move", Path.Combine(emuDir, "core_move.cpp"), Path.Combine(buildDir, "rv32emu_move.dll")),
};

Console.WriteLine("=== Step 1: build all emulators (parallel) ===");
Parallel.ForEach(cores, c => {
    Console.WriteLine($"-- building {c.Name}");
    Run("clang++", "-O2", "-std=c++20", "-shared",
        "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
        c.Src, "-o", c.Dll);
});

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

var hostCrc = Crc32("Hello, RV32I!");
Console.WriteLine($"Host CRC32(\"Hello, RV32I!\") = 0x{hostCrc:x8}\n");

// ============================================================
// Parallel run + interactive TUI
// ============================================================

var rs = cores.Select(c => new CoreResult { Name = c.Name, Dll = c.Dll }).ToArray();
var tasks = rs.Select(r => Task.Run(() => RunCore(r, image, hostCrc))).ToArray();

Console.WriteLine("=== Parallel run — ↑/↓ select, ←/→ or SPACE expand, PgUp/PgDn/Home/End scroll, Q quit ===");
Console.WriteLine();
// Detect a real interactive console; if stdout is redirected, skip the TUI.
bool hasTui = !Console.IsOutputRedirected && !Console.IsInputRedirected;
int tuiTop = 0;
if (hasTui) {
    try {
        int reserve = Math.Min(Console.BufferHeight - 1, rs.Length * 4 + 4);
        for (int i = 0; i < reserve; i++) Console.WriteLine();
        Console.SetCursorPosition(0, Math.Max(0, Console.CursorTop - reserve));
        tuiTop = Console.CursorTop;
    } catch { hasTui = false; }
}
int selected = 0;
var expanded = new bool[rs.Length];
int lastRendered = 0;
int scrollOff = 0;
int totalLines = 0;

if (hasTui) {
    try { Console.CursorVisible = false; } catch { }
    while (true) {
        var rr = Render(rs, selected, expanded, tuiTop, lastRendered, scrollOff);
        lastRendered = rr.written;
        totalLines = rr.total;
        // Clamp scroll offset if expansion collapsed shrunk content.
        int pageSize = Math.Max(1, Console.BufferHeight - tuiTop - 2);
        int maxScroll = Math.Max(0, totalLines - pageSize);
        if (scrollOff > maxScroll) { scrollOff = maxScroll; continue; }

        bool allDone = tasks.All(t => t.IsCompleted);

        if (Console.KeyAvailable) {
            var k = Console.ReadKey(true);
            if      (k.Key == ConsoleKey.UpArrow)   selected = (selected - 1 + rs.Length) % rs.Length;
            else if (k.Key == ConsoleKey.DownArrow) selected = (selected + 1) % rs.Length;
            else if (k.Key == ConsoleKey.Spacebar)   { expanded[selected] = !expanded[selected]; scrollOff = 0; }
            else if (k.Key == ConsoleKey.RightArrow) { expanded[selected] = true;  scrollOff = 0; }
            else if (k.Key == ConsoleKey.LeftArrow)  { expanded[selected] = false; scrollOff = 0; }
            else if (k.Key == ConsoleKey.PageUp)    scrollOff = Math.Max(0, scrollOff - pageSize);
            else if (k.Key == ConsoleKey.PageDown)  scrollOff = Math.Min(maxScroll, scrollOff + pageSize);
            else if (k.Key == ConsoleKey.Home)      scrollOff = 0;
            else if (k.Key == ConsoleKey.End)       scrollOff = maxScroll;
            else if (k.Key == ConsoleKey.Q || k.Key == ConsoleKey.Enter || k.Key == ConsoleKey.Escape) {
                if (allDone) break;
            }
        } else {
            Thread.Sleep(60);
        }
    }
} else {
    // Non-interactive: just wait for completion, then print a static summary.
    Task.WaitAll(tasks);
    foreach (var r in rs) {
        string status = r.Status == "done"
            ? (r.CrcOk ? "[ \x1b[32m✓\x1b[0m valid    ]" : "[ \x1b[31m✗\x1b[0m FAILED   ]")
            : (r.Status == "failed" ? "[ \x1b[31m✗\x1b[0m FAILED   ]" : $"[ · {r.Status,-8} ]");
        string fabricStr = r.Fabric > 0 ? $"{r.Fabric,14:N0} fab.cyc" : new string(' ', 22);
        Console.WriteLine($"  {r.Name,-5} {status} PC=0x{r.Pc:x8}  {r.Ms,7:F1} ms  {r.Cycles,12:N0} cycles  {r.Uops,14:N0} uops  {fabricStr}");
    }
}

// Make sure tasks have finished (Q only exits when allDone, but be defensive).
Task.WaitAll(tasks);

// Move cursor below the TUI block so subsequent prints don't overlap.
if (hasTui) {
    try { Console.SetCursorPosition(0, tuiTop + lastRendered); } catch { }
    try { Console.CursorVisible = true; } catch { }
}
Console.WriteLine();

// ============================================================
// Cross-core correctness & CRC checks
// ============================================================

Console.WriteLine("=== Cross-core correctness ===");
var baseline = rs[0];
bool allMatch = true;
foreach (var r in rs.Skip(1)) {
    bool pcEq  = r.Pc == baseline.Pc;
    bool regEq = MemEq(r.Regs, baseline.Regs);
    bool outEq = MemEq(r.Out,  baseline.Out);
    Console.WriteLine($"  {baseline.Name} vs {r.Name}: PC={(pcEq?"OK":"DIFF")}  regs={(regEq?"OK":"DIFF")}  out={(outEq?"OK":"DIFF")}");
    if (!regEq) {
        for (int i = 0; i < 32; i++)
            if (r.Regs[i] != baseline.Regs[i])
                Console.WriteLine($"    x{i,-2} {baseline.Name}=0x{baseline.Regs[i]:x8}  {r.Name}=0x{r.Regs[i]:x8}");
    }
    allMatch &= pcEq && regEq && outEq;
}

Console.WriteLine();
Console.WriteLine("=== CRC32 check per core ===");
const string MARK = "crc32(\"Hello, RV32I!\") = 0x";
bool allCrc = true;
foreach (var r in rs) {
    int idx = r.Text.IndexOf(MARK, StringComparison.Ordinal);
    if (idx < 0) { Console.WriteLine($"  {r.Name}: (no CRC line)"); allCrc = false; continue; }
    var hex = r.Text.Substring(idx + MARK.Length, 8);
    uint gCrc = Convert.ToUInt32(hex, 16);
    bool ok = gCrc == hostCrc;
    Console.WriteLine($"  {r.Name}: guest=0x{gCrc:x8}  {(ok ? "MATCH" : "MISMATCH")}");
    allCrc &= ok;
}

// Cycle-count consistency: every core executes the same RV32I program,
// so the retired-instruction count must be identical.
Console.WriteLine();
Console.WriteLine("=== Cycle-count consistency ===");
bool allCycEq = rs.All(r => r.Cycles == rs[0].Cycles);
foreach (var r in rs)
    Console.WriteLine($"  {r.Name}: {r.Cycles:N0} cycles  {(r.Cycles == rs[0].Cycles ? "OK" : "DIFF")}");
Console.WriteLine(allCycEq ? "  all cores executed identical instruction streams" : "  CYCLE MISMATCH");

Console.WriteLine();
Console.WriteLine((allMatch && allCrc && allCycEq)
    ? "ALL CORES BIT-IDENTICAL, CRC OK, CYCLES EQUAL"
    : "DIVERGENCE DETECTED");

return (allMatch && allCrc && allCycEq) ? 0 : 1;

// ============================================================
// Helpers
// ============================================================

static (int written, int total) Render(CoreResult[] rs, int selected, bool[] expanded, int top, int lastRendered, int scrollOff)
{
    var lines = new List<string>();
    int width = Math.Max(40, Console.WindowWidth - 1);

    const string GREEN = "\x1b[32m", RED = "\x1b[31m", RESET = "\x1b[0m";
    for (int i = 0; i < rs.Length; i++) {
        var r = rs[i];
        string marker = (i == selected) ? "▶" : " ";
        // Plain status cell with a single-char symbol placeholder (· for
        // queued/running). Symbol is the only thing colored — the rest is
        // plain text so column widths stay aligned.
        string sym;        // single character
        string label;
        string symColor = "";
        if (r.Status == "done") {
            bool ok = r.CrcOk;
            sym = ok ? "✓" : "✗";
            label = ok ? "valid   " : "FAILED  ";
            symColor = ok ? GREEN : RED;
        } else if (r.Status == "failed") {
            sym = "✗"; label = "FAILED  "; symColor = RED;
        } else if (r.Status == "running") {
            sym = "·"; label = "running ";
        } else {
            sym = "·"; label = "queued  ";
        }
        string statusPlain = $"[ {sym} {label} ]";
        string body = r.Status == "done"
            ? $"PC=0x{r.Pc:x8}  {r.Ms,7:F1} ms  {r.Cycles,12:N0} cycles  {r.Uops,14:N0} uops  " + (r.Fabric > 0 ? $"{r.Fabric,14:N0} fab.cyc" : new string(' ', 22))
            : (r.Status == "failed" ? r.Error ?? "(error)" : "...");
        string row = $"{marker} {r.Name,-5} {statusPlain} {body}";
        string padded = Pad(row, width);
        // Wrap only the single-char symbol with color, after padding so the
        // visible width is unchanged.
        string colored = padded;
        if (symColor.Length > 0) {
            int sIdx = padded.IndexOf(sym, StringComparison.Ordinal);
            if (sIdx >= 0)
                colored = padded.Substring(0, sIdx) + symColor + sym + RESET + padded.Substring(sIdx + sym.Length);
        }
        lines.Add(colored);

        if (expanded[i]) {
            foreach (var raw in (r.FullLog ?? "").Split('\n')) {
                lines.Add(Pad("      " + raw.TrimEnd('\r'), width));
            }
            if (r.Status == "done") {
                lines.Add(Pad($"      ── guest output ── ({r.Cycles:N0} cycles, {r.Ms:F1} ms)", width));
                foreach (var raw in r.Text.Split('\n')) {
                    lines.Add(Pad("        " + raw.TrimEnd('\r'), width));
                }
                lines.Add(Pad($"      ── cycles used: {r.Cycles:N0} ──", width));
            }
        }
    }

    int total = lines.Count;

    // Window the line list using scrollOff, then reserve a footer row for
    // scroll indicators when there's more content above/below the view.
    int bufH = Console.BufferHeight;
    if (top >= bufH) top = Math.Max(0, bufH - 1);
    int avail = Math.Max(0, bufH - top - 1);
    int pageSize = Math.Max(1, avail - 1);   // -1 leaves room for footer

    if (scrollOff < 0) scrollOff = 0;
    if (scrollOff > Math.Max(0, total - pageSize)) scrollOff = Math.Max(0, total - pageSize);

    var view = (scrollOff < total)
        ? lines.GetRange(scrollOff, Math.Min(pageSize, total - scrollOff))
        : new List<string>();

    bool more_above = scrollOff > 0;
    bool more_below = scrollOff + view.Count < total;
    string footer =
        (more_above || more_below)
            ? $"  ── lines {scrollOff + 1}-{scrollOff + view.Count} of {total}  "
              + (more_above ? "↑ PgUp " : "       ")
              + (more_below ? "↓ PgDn " : "       ")
              + "(Home/End)"
            : "";
    view.Add(Pad(footer, width));

    // Pad to overwrite any previously-rendered trailing lines.
    while (view.Count < lastRendered && view.Count < avail) view.Add(Pad("", width));

    if (view.Count == 0) return (0, total);
    Console.SetCursorPosition(0, top);
    for (int i = 0; i < view.Count; i++) {
        Console.SetCursorPosition(0, top + i);
        Console.Write(view[i]);
    }
    return (view.Count, total);
}

static string Pad(string s, int w) {
    if (s.Length >= w) return s.Substring(0, w);
    return s + new string(' ', w - s.Length);
}

[DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
static extern uint GetModuleFileName(IntPtr hModule, [Out] System.Text.StringBuilder lpFilename, uint nSize);

static unsafe void RunCore(CoreResult r, byte[] image, uint hostCrc)
{
    var log = new StringBuilder();
    try {
        r.Status = "running";
        var h = NativeLibrary.Load(r.Dll);
        var sb = new System.Text.StringBuilder(1024);
        GetModuleFileName(h, sb, (uint)sb.Capacity);
        var loadedPath = sb.ToString();
        var createPtr  = NativeLibrary.GetExport(h, "core_create");
        log.AppendLine($"loaded handle = 0x{h.ToInt64():x16}");
        log.AppendLine($"loaded path   = {loadedPath}");
        log.AppendLine($"expected path = {r.Dll}");
        log.AppendLine($"core_create   = 0x{createPtr.ToInt64():x16}");
        if (!string.Equals(Path.GetFullPath(loadedPath), Path.GetFullPath(r.Dll), StringComparison.OrdinalIgnoreCase))
            throw new Exception($"DLL identity mismatch: expected {r.Dll}, got {loadedPath}");

        var create  = (delegate* unmanaged[Cdecl]<IntPtr, uint, IntPtr>) createPtr;
        var step    = (delegate* unmanaged[Cdecl]<IntPtr, int>)          NativeLibrary.GetExport(h, "core_step");
        var destroy = (delegate* unmanaged[Cdecl]<IntPtr, void>)         NativeLibrary.GetExport(h, "core_destroy");
        var getPc   = (delegate* unmanaged[Cdecl]<IntPtr, uint>)         NativeLibrary.GetExport(h, "core_get_pc");
        var getReg  = (delegate* unmanaged[Cdecl]<IntPtr, int, uint>)    NativeLibrary.GetExport(h, "core_get_reg");

        IntPtr statsPtr   = TryGet(h, "core_get_stats");
        IntPtr twStatsPtr = TryGet(h, "core_tw_stats");
        IntPtr uopsPtr    = TryGet(h, "core_get_uops");
        IntPtr fabricPtr  = TryGet(h, "core_get_fabric_cycles");

        var mem = new byte[MEM_SIZE];
        Array.Copy(image, 0, mem, 0, image.Length);

        uint pc;
        var regs = new uint[32];
        ulong cycles = 0;
        ulong uopsCount = 0;
        ulong fabricCount = 0;
        var sw = Stopwatch.StartNew();
        fixed (byte* p = mem) {
            IntPtr core = create((IntPtr)p, 0);
            // Step in a loop so we get a uniform cycle count across cores.
            while (step(core) != 0) cycles++;
            sw.Stop();
            pc = getPc(core);
            for (int i = 0; i < 32; i++) regs[i] = getReg(core, i);
            if (uopsPtr != IntPtr.Zero) {
                var getUops = (delegate* unmanaged[Cdecl]<IntPtr, ulong>) uopsPtr;
                uopsCount = getUops(core);
            }
            if (fabricPtr != IntPtr.Zero) {
                var getFabric = (delegate* unmanaged[Cdecl]<IntPtr, ulong>) fabricPtr;
                fabricCount = getFabric(core);
            }
            destroy(core);
        }

        if (statsPtr != IntPtr.Zero) {
            var getStats = (delegate* unmanaged[Cdecl]<ulong*, ulong*, ulong*, ulong*, ulong*, void>) statsPtr;
            ulong w1=0, w2=0, w4=0, w8=0, ins=0;
            getStats(&w1, &w2, &w4, &w8, &ins);
            log.AppendLine("TTA fusion stats:");
            log.AppendLine($"  RV32I instructions executed : {ins:N0}");
            log.AppendLine($"  width-1 bundles (baseline)  : {w1:N0}  ({(double)w1/ins:F1} per insn)");
            log.AppendLine($"  width-2 bundles             : {w2:N0}  ({100.0*(w1-w2)/w1:F1}% reduction)");
            log.AppendLine($"  width-4 bundles             : {w4:N0}  ({100.0*(w1-w4)/w1:F1}% reduction)");
            log.AppendLine($"  width-8 bundles             : {w8:N0}  ({100.0*(w1-w8)/w1:F1}% reduction)");
        }
        if (twStatsPtr != IntPtr.Zero) {
            var getTwStats = (delegate* unmanaged[Cdecl]<ulong*, ulong*, ulong*, ulong*, ulong*, void>) twStatsPtr;
            ulong w1=0, w4=0, w8=0, edges=0, ins=0;
            getTwStats(&w1, &w4, &w8, &edges, &ins);
            log.AppendLine("TW trigger-word stats:");
            log.AppendLine($"  RV32I instructions executed : {ins:N0}");
            log.AppendLine($"  width-1 bundles             : {w1:N0}  ({(double)w1/ins:F1} per insn)");
            if (edges > 0) {
                log.AppendLine($"  sampled fusion (first 128 insns):");
                log.AppendLine($"    edges discovered : {edges:N0}");
                log.AppendLine($"    width-4 fused    : {w4:N0}");
                log.AppendLine($"    width-8 fused    : {w8:N0}");
            }
        }

        var outBytes = new byte[OUT_END - OUT_BASE];
        Array.Copy(mem, (int)OUT_BASE, outBytes, 0, outBytes.Length);
        int n = 0; while (n < outBytes.Length && outBytes[n] != 0) n++;
        var text = Encoding.ASCII.GetString(outBytes, 0, n);

        NativeLibrary.Free(h);

        r.Pc = pc;
        r.Regs = regs;
        r.Out = outBytes;
        r.Text = text;
        r.Ms = sw.Elapsed.TotalMilliseconds;
        r.Cycles = cycles;
        r.Uops = uopsCount;
        r.Fabric = fabricCount;
        r.FullLog = log.ToString();

        // Per-core verification: extract the embedded guest CRC line and
        // compare to the host-computed CRC for the same string.
        const string MARK = "crc32(\"Hello, RV32I!\") = 0x";
        int idx = text.IndexOf(MARK, StringComparison.Ordinal);
        if (idx >= 0 && idx + MARK.Length + 8 <= text.Length) {
            try {
                uint guestCrc = Convert.ToUInt32(text.Substring(idx + MARK.Length, 8), 16);
                r.GuestCrc = guestCrc;
                r.CrcOk = (guestCrc == hostCrc);
            } catch { r.CrcOk = false; }
        } else {
            r.CrcOk = false;
        }
        r.Status = "done";
    } catch (Exception ex) {
        r.Error = ex.Message;
        r.FullLog = log.ToString() + "\n" + ex;
        r.Status = "failed";
    }
}

static IntPtr TryGet(IntPtr h, string name) {
    try { return NativeLibrary.GetExport(h, name); } catch { return IntPtr.Zero; }
}

static bool MemEq<T>(T[] a, T[] b) where T : IEquatable<T>
{
    if (a.Length != b.Length) return false;
    for (int i = 0; i < a.Length; i++) if (!a[i].Equals(b[i])) return false;
    return true;
}

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
    using var p = Process.Start(psi)!;
    string stdout = p.StandardOutput.ReadToEnd();
    string stderr = p.StandardError.ReadToEnd();
    p.WaitForExit();
    if (p.ExitCode != 0) {
        Console.Error.WriteLine(stdout);
        Console.Error.WriteLine(stderr);
        throw new Exception($"{exe} exited with code {p.ExitCode}");
    }
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

class CoreResult {
    public string  Name = "";
    public string  Dll = "";
    public string  Status = "queued";
    public string? Error;
    public uint    Pc;
    public uint[]  Regs = new uint[32];
    public byte[]  Out  = Array.Empty<byte>();
    public string  Text = "";
    public double  Ms;
    public ulong   Cycles;
    public ulong   Uops;
    public ulong   Fabric;
    public uint    GuestCrc;
    public bool    CrcOk;
    public string  FullLog = "";
}
