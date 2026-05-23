using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class AtomicTrapTest : EmulatorTestBase
{
    [TestMethod]
    public void AtomicOpcodeTrapsAsIllegalInstruction()
    {
        string srcFile = Path.Combine(ProgramDir, "atomic_trap.c");
        string elfFile = Path.Combine(TestDir, "atomic_trap.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the atomic trap test.");
        Assert.AreEqual(0, exitCode, "A-extension opcode did not trap to the installed handler.");
    }
}
