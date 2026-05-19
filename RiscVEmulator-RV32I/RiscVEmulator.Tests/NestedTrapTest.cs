using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class NestedTrapTest : EmulatorTestBase
{
    [TestMethod]
    public void NestedTrapDoesNotCorruptTheOuterContext()
    {
        string srcFile = Path.Combine(ProgramDir, "nested_trap.c");
        string elfFile = Path.Combine(TestDir, "nested_trap.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the nested-trap test.");
        Assert.AreEqual(0, exitCode,
            "a nested trap corrupted the outer trap frame — the asymmetric " +
            "landing-pad / private-frame design is broken.");
    }
}
