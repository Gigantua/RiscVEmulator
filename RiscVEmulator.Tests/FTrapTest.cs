using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FTrapTest : EmulatorTestBase
{
    [TestMethod]
    public void FOpcodeTrapsAsIllegalInstruction()
    {
        string srcFile = Path.Combine(ProgramDir, "f_trap.c");
        string elfFile = Path.Combine(TestDir, "f_trap.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the F trap test.");
        Assert.AreEqual(0, exitCode, "F-extension opcode did not trap to the installed handler.");
    }
}

