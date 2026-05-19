using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class DirectUserEcallTest : EmulatorTestBase
{
    [TestMethod]
    public void DirectUserEcallRaisesEnvironmentCallTrap()
    {
        string srcFile = Path.Combine(ProgramDir, "direct_user_ecall.c");
        string elfFile = Path.Combine(TestDir, "direct_user_ecall.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the direct user ECALL test.");
        Assert.AreEqual(0, exitCode, "direct user ECALL did not raise cause 8 through mtvec.");
    }
}
