using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class TimerTest : EmulatorTestBase
{
    [TestMethod]
    public void TimerAdvancesAndIsReadable()
    {
        string output = RunProgramWithLibc("timer_test");
        string[] lines = output.Split('\n', StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual("timer_test", lines[0]);

        // t0 and t1 are printed as numbers — just verify they exist
        Assert.IsTrue(lines[1].StartsWith("t0="), $"Line 1: {lines[1]}");
        Assert.IsTrue(lines[2].StartsWith("t1="), $"Line 2: {lines[2]}");
        Assert.IsTrue(lines[3].StartsWith("elapsed="), $"Line 3: {lines[3]}");

        // Parse elapsed and verify it's positive
        string elapsedStr = lines[3].Substring("elapsed=".Length);
        Assert.IsTrue(uint.TryParse(elapsedStr, out uint elapsed), $"Cannot parse elapsed: {elapsedStr}");
        Assert.IsTrue(elapsed > 0, $"elapsed should be > 0, was {elapsed}");
        Assert.IsTrue(elapsed > 100, $"elapsed should be > 100 (busy loop), was {elapsed}");

        Assert.AreEqual("mtime_advances: OK", lines[4]);
        Assert.AreEqual("mtime_reasonable: OK", lines[5]);
        Assert.AreEqual("mtime_hi_zero: OK", lines[6]);

        Assert.AreEqual(7, lines.Length, $"Expected 7 lines, got {lines.Length}");
    }
}
