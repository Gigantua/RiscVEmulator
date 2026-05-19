using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class SModeRemovalTest : EmulatorTestBase
{
    [TestMethod]
    public void SModeInstructionsAndCsrsTrapAsIllegalInstruction()
    {
        string srcFile = Path.Combine(ProgramDir, "s_mode_removed.c");
        string elfFile = Path.Combine(TestDir, "s_mode_removed.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the removed-privilege test.");
        Assert.AreEqual(0, exitCode, "Removed privileged opcode/CSR access did not trap as illegal instruction.");
    }
}
