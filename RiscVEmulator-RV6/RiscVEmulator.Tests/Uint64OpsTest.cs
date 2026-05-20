using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class Uint64OpsTest : EmulatorTestBase
{
    private static readonly string[] ExpectedLines =
    {
        "add=00000001:00000000",
        "sub=00000000:FFFFFFFF",
        "mul=00000001:00000000",
        "mul2=00000002:540BE400",
        "xor=FFFFFFFF:FFFFFFFF",
        "and=0F000F00:0F000F00",
        "or=FF000000:0000FF00",
        "shl=00000001:00000000",
        "shr=00000000:80000000",
        "shl1=00000001:00000000",
        "not=FFFFFFFF:FFFFFFFF",
        "neg=FFFFFFFF:FFFFFFFF",
    };

    [TestMethod]
    public void Uint64OperationsProduceCorrectResults()
    {
        string text = RunProgramWithLibc("uint64_ops");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
