using System.Diagnostics;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Real-wall-clock FPS measurement for the bare-metal Quake build.
///
/// Boots the binary, waits for `cls.state=ca_active` (signon stage 4 —
/// the moment the demo loop or e1m1 starts rendering), then times how
/// many vsync presents the framebuffer accumulates over the next
/// fixed number of guest instructions. Reports:
///   - host wall-clock elapsed
///   - guest instructions executed (MIPS)
///   - vsync-present count (real Quake fps)
///
/// This is the apples-to-apples way to measure perf wins from JIT /
/// softfloat / compile-flag changes.
/// </summary>
[TestClass]
public class QuakeFpsBenchTest
{
    public TestContext? TestContext { get; set; }

    private static (string elf, string pak) ResolveArtefacts()
    {
        string dir = AppContext.BaseDirectory;
        string? root = null;
        for (var d = new DirectoryInfo(dir); d != null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "Examples", "Quake")))
            { root = d.FullName; break; }
        root ??= dir;
        string[] elfC =
        {
            Path.Combine(root, "Examples", "Quake", "bin", "x64", "Release", "net10.0", "build", "quake.elf"),
            Path.Combine(root, "Examples", "Quake", "bin", "Release", "net10.0", "build", "quake.elf"),
        };
        string[] pakC =
        {
            Path.Combine(root, "Examples", "Quake", "bin", "x64", "Release", "net10.0", "id1", "pak0.pak"),
            Path.Combine(root, "Examples", "Quake", "bin", "Release", "net10.0", "id1", "pak0.pak"),
            Path.Combine(root, "Examples", "Quake", "id1", "pak0.pak"),
        };
        return (elfC.FirstOrDefault(File.Exists) ?? elfC[0],
                pakC.FirstOrDefault(File.Exists) ?? pakC[0]);
    }

    [TestMethod]
    [TestCategory("LongRunning")]
    public void MeasureFps()
    {
        var (elfPath, pakPath) = ResolveArtefacts();
        if (!File.Exists(elfPath) || !File.Exists(pakPath))
            Assert.Inconclusive($"Quake artefacts not built.\n  elf: {elfPath}\n  pak: {pakPath}");

        // Mirror Examples/Quake/Program.cs SoC.
        var memory  = new Memory(64 * 1024 * 1024);
        var bus     = new MemoryBus(memory);
        var uart    = new UartDevice();
        var fb      = new FramebufferDevice();
        var display = new DisplayControlDevice(fb);
        display.SetMemory(memory);
        var kbd       = new KeyboardDevice();
        var mouse     = new MouseDevice();
        var rtc       = new RealTimeClockDevice();
        var audioBuf  = new AudioBufferDevice();
        var audioCtrl = new AudioControlDevice();
        var clint     = new ClintDevice();
        var trapFrame = new TrapFrameDevice();
        var disk      = new DiskDevice();
        bus.RegisterPeripheral(uart);
        bus.RegisterPeripheral(fb);
        bus.RegisterPeripheral(display);
        bus.RegisterPeripheral(kbd);
        bus.RegisterPeripheral(mouse);
        bus.RegisterPeripheral(rtc);
        bus.RegisterPeripheral(audioBuf);
        bus.RegisterPeripheral(audioCtrl);
        bus.RegisterPeripheral(clint);
        bus.RegisterPeripheral(trapFrame);
        bus.RegisterPeripheral(disk);

        byte[] elfData = File.ReadAllBytes(elfPath);
        byte[] pakData = File.ReadAllBytes(pakPath);
        disk.LoadFile(pakData, bus);

        var regs   = new RegisterFile();
        uint entry = ElfLoader.Load(elfData, bus);
        regs.Write(2, 0x03FFFFF0u);

        // Install SSE softfloat hooks (matches Examples/Quake/Program.cs).
        var sfSyms = ElfSymbols.ReadFunctionSymbols(elfData);
        int sfHooks = Emulator.InstallSoftFloatHooks(sfSyms);

        var sb  = new StringBuilder();
        var emu = new Emulator(bus, regs, entry);
        emu.OutputHandler  = _ => { };
        uart.OutputHandler = c => sb.Append(c);

        // ── Phase 1: boot until something renders (vsync count > 0).
        // Cap at 80 G steps so a broken binary doesn't hang the test.
        var bootSw = Stopwatch.StartNew();
        const int batch = 100_000_000;
        long bootSteps = 0;
        const long bootCap = 80_000_000_000L;
        while (bootSteps < bootCap && display.VsyncCount == 0 && !emu.IsHalted)
        {
            emu.Run(batch);
            bootSteps += batch;
        }
        bootSw.Stop();

        if (display.VsyncCount == 0)
            Assert.Fail($"Quake never rendered a frame in {bootSteps:N0} steps.\nLog tail:\n{TailString(sb, 2000)}");

        // ── Phase 2: timed measurement window.
        // Run for a fixed wall-clock duration and count guest steps + vsyncs.
        const int measureMillis = 10_000;
        ulong  vsyncStart   = display.VsyncCount;
        long   stepsStart   = bootSteps;
        long   stepsTotal   = bootSteps;
        var    measureSw    = Stopwatch.StartNew();
        while (measureSw.ElapsedMilliseconds < measureMillis && !emu.IsHalted)
        {
            emu.Run(batch);
            stepsTotal += batch;
        }
        measureSw.Stop();

        ulong vsyncEnd      = display.VsyncCount;
        long  stepsMeasured = stepsTotal - stepsStart;
        ulong vsyncs        = vsyncEnd - vsyncStart;
        double elapsedSec   = measureSw.Elapsed.TotalSeconds;
        double fps          = vsyncs / elapsedSec;
        double mips         = stepsMeasured / 1e6 / elapsedSec;

        string report =
            $"\n=== Quake FPS benchmark ===\n" +
            $"Boot      : {bootSteps:N0} steps in {bootSw.Elapsed.TotalSeconds:F2}s\n" +
            $"Softfloat : {sfHooks} SSE hooks installed\n" +
            $"Window    : {elapsedSec:F2}s\n" +
            $"Steps     : {stepsMeasured:N0}\n" +
            $"MIPS      : {mips:F1}\n" +
            $"Vsyncs    : {vsyncs}\n" +
            $"FPS       : {fps:F2}\n";
        TestContext?.WriteLine(report);
        Console.WriteLine(report);

        // Sanity floor — if FPS drops below 1 it means rendering stalled.
        Assert.IsTrue(fps >= 1.0,
            $"Quake FPS dropped below 1.0 — likely a stall.\n{report}\nLog tail:\n{TailString(sb, 2000)}");
    }

    private static string TailString(StringBuilder sb, int chars)
    {
        if (sb.Length <= chars) return sb.ToString();
        return sb.ToString(sb.Length - chars, chars);
    }
}
