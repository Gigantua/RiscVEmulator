using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class ParavirtSyscallGatewayTest : EmulatorTestBase
{
    [TestMethod]
    public void ParavirtSyscallGatewayRaisesEnvironmentCallTrap()
    {
        string srcFile = Path.Combine(ProgramDir, "paravirt_syscall_gateway.c");
        string elfFile = Path.Combine(TestDir, "paravirt_syscall_gateway.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 1_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the paravirt syscall gateway test.");
        Assert.AreEqual(0, exitCode, "paravirt syscall gateway did not raise a precise environment-call trap.");
    }
}

