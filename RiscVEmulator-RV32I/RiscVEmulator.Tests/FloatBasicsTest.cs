using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FloatBasicsTest : EmulatorTestBase
{
    private static readonly string[] ExpectedLines =
    {
        "add=4",
        "sub=7",
        "mul=12",
        "div=3",
        "div2=3",
        "div3=3",
        "neg_add=-2",
        "neg_mul=12",
        "i2f=42",
        "f2i=7",
        "neg_f2i=-3",
        "lt=1",
        "gt=0",
        "eq=1",
        "ge=1",
        "le=1",
        "ne=1",
    };

    [TestMethod]
    public void FloatScalarArithmeticAndComparisons()
    {
        string text = RunProgramWithLibc("float_basics");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
