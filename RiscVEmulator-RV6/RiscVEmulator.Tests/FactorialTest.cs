using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FactorialTest : EmulatorTestBase
{
    // 20! = 2432902008176640000 = 0x21C3677C:82B40000
    private static readonly string[] ExpectedLines =
    {
        "1!=00000000:00000001",
        "5!=00000000:00000078",
        "10!=00000000:00375F00",
        "13!=00000001:7328CC00",
        "20!=21C3677C:82B40000",
        "20!dec=2432902008176640000",
    };

    [TestMethod]
    public void RecursiveFactorial20IsCorrect()
    {
        // factorial.c needs runtime.c for __udivdi3/__umoddi3 (decimal printing)
        string text = RunProgramWithLibc("factorial", maxSteps: 50_000_000);
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
