using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class ArxBuiltinTest : EmulatorTestBase
{
    /// <summary>
    /// Phase 1 acceptance: clang compiles <c>__builtin_riscv_arx(...)</c> into
    /// the ARX custom-0 instruction, the CPU decodes it (case 0x0B), and the
    /// four canonical reductions — rotate+xor, pure rotate, pure XOR, pure
    /// ADD — produce the expected values.
    ///
    /// The ELF must be built ahead of time with the patched WSL clang:
    ///   wsl ~/llvm-build/bin/clang --target=riscv32-unknown-elf \
    ///       -march=rv32i_xarx -mabi=ilp32 -nostdlib -O2 -ffreestanding \
    ///       -static -Wl,-Ttext=0 -fuse-ld=lld \
    ///       Programs/arx_smoke.c -o %TEMP%/arx_smoke.elf
    /// </summary>
    [TestMethod]
    public void ArxBuiltinEndToEnd()
    {
        string elf = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "arx_smoke.elf");
        if (!System.IO.File.Exists(elf))
            Assert.Inconclusive(
                $"Build the ELF first with the patched WSL clang and place it at {elf}. " +
                "See the test's doc-comment for the exact command.");

        var fullResult = RunElfFull(elf, maxSteps: 20_000_000);
        string output = fullResult.Output;
        int    exitCode = fullResult.ExitCode;
        bool   halted   = fullResult.Halted;
        System.Console.WriteLine($"DIAG: halted={halted} exit={exitCode} sbLen={output.Length} content='{output}'");
        // Diagnostic: peek at the UART peripheral to see if writes happened
        // at all (helps distinguish MMIO loss from drain-thread race).
        var uart = fullResult.Bus.Peripherals
            .OfType<RiscVEmulator.Core.Peripherals.UartDevice>()
            .First();
        System.Console.WriteLine($"DIAG: ThrWriteCount={uart.ThrWriteCount}");
        Assert.IsTrue(halted, "Emulator did not halt; output so far:\n" + output);
        Assert.AreEqual(0, exitCode, "Non-zero exit code; output:\n" + output);
        StringAssert.Contains(output, "r0=0x0000010f", "rotate+xor result wrong: " + output);
        StringAssert.Contains(output, "r1=0x00000010", "pure rotate result wrong: " + output);
        StringAssert.Contains(output, "r2=0x000000ff", "pure XOR result wrong: " + output);
        StringAssert.Contains(output, "r3=0x00001234", "pure ADD result wrong: " + output);
        StringAssert.Contains(output, "ARX OK", "ARX guest summary missing: " + output);
    }
}
