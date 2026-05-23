using System.Diagnostics;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Throughput benchmarks — each kernel runs 3× in fresh Emulator instances;
/// we report best MIPS to filter cold-start / CPU-frequency noise. The
/// backend in use is whatever <c>RVEMU_JIT</c> selects (0=interpreter,
/// anything else = the hand-rolled JIT); both backends are exercised by
/// running the same test under each env-var setting.
/// </summary>
[TestClass]
public class JitBenchmarkTest : EmulatorTestBase
{
    private const int Repeats = 3;

    // ── Numeric benchmarks ────────────────────────────────────────────

    [TestMethod] public void BenchmarkMips()           => RunBench("bench",        "result=");
    [TestMethod] public void BenchmarkMipsPrimes()     => RunBench("primes",       "Done.");
    [TestMethod] public void BenchmarkMatmul()         => RunBench("bench_matmul", "trace=");
    [TestMethod] public void BenchmarkCrc()            => RunBench("bench_crc",    "crc=");
    [TestMethod] public void BenchmarkSoftFp()         => RunBench("bench_softfp", "fp=");

    private static void RunBench(string baseName, string mustContain)
    {
        string srcFile = Path.Combine(ProgramDir, $"{baseName}.c");
        string elfFile = Path.Combine(TestDir, $"{baseName}.elf");
        string rtObj   = EnsureRuntimeObject();
        string sfObj   = EnsureSoftFloatObject();
        string scObj   = EnsureSyscallsObject();
        string lcObj   = EnsureLibcObject();
        CompileC(new[] { srcFile, lcObj, sfObj, scObj, rtObj }, elfFile);
        byte[] elfData = File.ReadAllBytes(elfFile);

        double bestMips  = 0;
        long   stepsLast = 0;
        var    runs      = new List<double>();

        for (int i = 0; i < Repeats; i++)
        {
            var memory  = new Memory(16 * 1024 * 1024);
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
            bus.RegisterPeripheral(uart); bus.RegisterPeripheral(fb);
            bus.RegisterPeripheral(display); bus.RegisterPeripheral(kbd);
            bus.RegisterPeripheral(mouse); bus.RegisterPeripheral(rtc);
            bus.RegisterPeripheral(audioBuf); bus.RegisterPeripheral(audioCtrl);
            bus.RegisterPeripheral(clint); bus.RegisterPeripheral(trapFrame);

            var regs   = new RegisterFile();
            uint entry = ElfLoader.Load(elfData, bus);
            regs.Write(2, 0x00800000u);

            var sb = new StringBuilder();
            uart.OutputHandler = c => sb.Append(c);
            using var emu = new Emulator(bus, regs, entry);
            emu.OutputHandler = c => sb.Append(c);

            var  sw    = Stopwatch.StartNew();
            long total = 0;
            while (!emu.IsHalted)
            {
                int r = emu.StepN(200_000_000);
                total += r;
                if (r == 0) break;
            }
            sw.Stop();

            Assert.IsTrue(emu.IsHalted, $"{baseName} run {i} did not halt");
            Assert.IsTrue(sb.ToString().Contains(mustContain),
                $"{baseName} run {i} bad output: {sb}");

            double secs = sw.Elapsed.TotalSeconds;
            double mips = total / secs / 1e6;
            runs.Add(mips);
            if (mips > bestMips) bestMips = mips;
            stepsLast = total;
        }

        string backend = Environment.GetEnvironmentVariable("RVEMU_JIT") == "0"
            ? "interp" : "hand-jit";
        string all = string.Join(", ", runs.ConvertAll(x => x.ToString("F0")));
        Console.WriteLine(
            $"[BENCH:{baseName,-13}] backend={backend,-9} steps={stepsLast,12} " +
            $"runs=[{all}] best={bestMips,7:F1} MIPS");
    }
}
