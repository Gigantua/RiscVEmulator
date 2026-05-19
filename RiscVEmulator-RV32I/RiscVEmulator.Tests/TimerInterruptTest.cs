using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class TimerInterruptTest : EmulatorTestBase
{
    [TestMethod]
    public void TimerInterruptDeliversThroughTheTrapFrame()
    {
        string srcFile = Path.Combine(ProgramDir, "timer_irq.c");
        string elfFile = Path.Combine(TestDir, "timer_irq.elf");

        CompileC(new[] { srcFile }, elfFile);

        var (_, exitCode, halted) = RunElf(elfFile, maxSteps: 2_000_000);
        Assert.IsTrue(halted, "emulator did not halt after the timer-interrupt test.");
        Assert.AreEqual(0, exitCode,
            "the CLINT machine-timer interrupt was not delivered through the " +
            "trap-frame page, or control did not return cleanly from the handler.");
    }
}
