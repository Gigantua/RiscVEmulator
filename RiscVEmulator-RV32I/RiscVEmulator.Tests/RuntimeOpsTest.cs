using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class RuntimeOpsTest : EmulatorTestBase
{
    private static readonly string[] ExpectedLines =
    {
        "runtime_ops",
        // 32-bit multiply
        "mul7x3=00000015 OK",
        "mul0=00000000 OK",
        "mulmax=FFFFFFFD OK",
        // 32-bit unsigned div/mod
        "udiv7/3=00000002 OK",
        "umod7%3=00000001 OK",
        "udiv/0=FFFFFFFF OK",
        "umod/0=00000007 OK",
        // 32-bit signed div/mod
        "sdiv7/3=00000002 OK",
        "sdivn7/3=FFFFFFFE OK",
        "smod7%3=00000001 OK",
        "smodn7%3=FFFFFFFF OK",
        "sdiv7/-1=FFFFFFF9 OK",
        // 64-bit multiply
        "mul64=00000002:540BE400 OK",
        // 64-bit unsigned div/mod
        "udiv64=00000021:42F30249 OK",   // 142857142857
        "umod64=00000000:00000001 OK",
        // 64-bit signed div/mod
        "sdiv64=FFFFFFDE:BD0CFDB7 OK",   // -142857142857
        "smod64=FFFFFFFF:FFFFFFFF OK",   // -1
        // 64-bit left shift
        "shl0=00000000:00000001 OK",
        "shl1=00000000:00000002 OK",
        "shl16=00000000:00010000 OK",
        "shl31=00000000:80000000 OK",
        "shl32=00000001:00000000 OK",
        "shl48=00010000:00000000 OK",
        "shl63=80000000:00000000 OK",
        // 64-bit logical right shift
        "lshr0=80000000:00000000 OK",
        "lshr1=40000000:00000000 OK",
        "lshr32=00000000:80000000 OK",
        "lshr63=00000000:00000001 OK",
        // 64-bit arithmetic right shift
        "ashr1=FFFFFFFF:FFFFFFFF OK",
        "ashr32=FFFFFFFF:80000000 OK",
        "ashr63=FFFFFFFF:FFFFFFFF OK",
        // memset/memcpy
        "memset=OK",
        "memcpy=OK",
        "runtime_done",
    };

    [TestMethod]
    public void AllRuntimeOperationsAreCorrect()
    {
        string text = RunProgramWithLibc("runtime_ops", maxSteps: 50_000_000);
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
