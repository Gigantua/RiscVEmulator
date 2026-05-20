using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FloatMatMulTest : EmulatorTestBase
{
    // float_matmul.c outputs 4 labelled 3x3 matrices, row-major, one integer per line.
    // AB = A*B, AI = A*I (=A), AA = A+A (=2A), 3A = 3*A
    private static readonly string[] ExpectedLines =
    {
        // A*B  [[ 30, 24, 18], [ 84, 69, 54], [138,114, 90]]
        "AB",
        "30", "24", "18",
        "84", "69", "54",
        "138", "114", "90",
        // A*I = A  [[1,2,3],[4,5,6],[7,8,9]]
        "AI",
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        // A+A = 2A  [[2,4,6],[8,10,12],[14,16,18]]
        "AA",
        "2", "4", "6",
        "8", "10", "12",
        "14", "16", "18",
        // 3A  [[3,6,9],[12,15,18],[21,24,27]]
        "3A",
        "3", "6", "9",
        "12", "15", "18",
        "21", "24", "27",
    };

    [TestMethod]
    public void FloatMatrixOperationsProduceCorrectResults()
    {
        string text = RunProgramWithLibc("float_matmul");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
