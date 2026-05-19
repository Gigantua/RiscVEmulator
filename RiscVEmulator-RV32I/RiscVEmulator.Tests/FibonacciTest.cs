using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FibonacciTest : EmulatorTestBase
{
    // Expected output lines in exact emission order
    private static readonly string[] ExpectedLines =
    {
        "fib(1)=1",  "fib(2)=1",   "fib(3)=2",   "fib(4)=3",
        "fib(5)=5",  "fib(6)=8",   "fib(7)=13",  "fib(8)=21",
        "fib(9)=34", "fib(10)=55", "fib(11)=89", "fib(12)=144",
        "fib(13)=233","fib(14)=377","fib(15)=610","fib(16)=987",
        "fib(17)=1597","fib(18)=2584","fib(19)=4181","fib(20)=6765",
    };

    [TestMethod]
    public void RecursiveFibonacciMatchesKnownValues()
    {
        string text = RunProgramWithLibc("fibonacci");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
