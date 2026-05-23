using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;

namespace RiscVEmulator.Tests;

/// <summary>
/// Compare softfloat-heavy code with and without the JIT's SSE shortcut.
/// Same ELF, same JIT — only difference is whether <see
/// cref="Emulator.InstallSoftFloatHooks"/> is called.
/// </summary>
[TestClass]
public class SoftFloatSseHookBenchTest : EmulatorTestBase
{
    public TestContext? TestContext { get; set; }

    [TestMethod]
    [TestCategory("LongRunning")]
    public void Microbench_ShortcutBeatsBaseline()
    {
        string srcFile = Path.Combine(ProgramDir, "softfloat_microbench.c");
        string elfFile = Path.Combine(TestDir, "softfloat_microbench.elf");
        Assert.IsTrue(File.Exists(srcFile));

        string rt  = EnsureRuntimeObject();
        string sf  = EnsureSoftFloatObject();
        CompileC(new[] { srcFile, sf, rt }, elfFile);

        double baselineMs = RunOnce(elfFile, installHooks: false);
        double hookedMs   = RunOnce(elfFile, installHooks: true);

        TestContext?.WriteLine($"baseline (no hooks): {baselineMs:F1} ms");
        TestContext?.WriteLine($"with SSE shortcut:   {hookedMs:F1} ms");
        TestContext?.WriteLine($"speedup:             {baselineMs / hookedMs:F2}x");

        // Hook must be measurably faster on this microbench (which is ~99%
        // softfloat). Floor at 1.5x — anything lower means the hook isn't
        // actually firing.
        Assert.IsTrue(hookedMs < baselineMs * 0.66,
            $"SSE hook didn't speed up softfloat-heavy code: baseline {baselineMs:F1}ms vs hooked {hookedMs:F1}ms.");
    }

    private static double RunOnce(string elfFile, bool installHooks)
    {
        byte[] data = File.ReadAllBytes(elfFile);
        // Uninstall any previously-set hooks first.
        for (int i = 1; i <= 8; i++)
            Emulator.SetSoftFloatHook((Emulator.SoftFloatOp)i, 0);

        if (installHooks)
        {
            var syms = ElfSymbols.ReadFunctionSymbols(data);
            int n = Emulator.InstallSoftFloatHooks(syms);
            Assert.IsTrue(n >= 2, $"expected ≥2 softfloat hooks installed, got {n}");
        }

        var (output, exitCode, halted) = RunElf(elfFile, maxSteps: 2_000_000_000);
        Assert.IsTrue(halted, $"didn't halt. Output: {output}");
        Assert.AreEqual(0, exitCode);

        var sw = Stopwatch.StartNew();
        var (_, _, _) = RunElf(elfFile, maxSteps: 2_000_000_000);
        sw.Stop();
        return sw.Elapsed.TotalMilliseconds;
    }
}
