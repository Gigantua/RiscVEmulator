using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class MathTest : EmulatorTestBase
{
    private static string EnsureMathObject()
    {
        string src = Path.Combine(RuntimeDir, "math.c");
        string obj = Path.Combine(TestDir, "math_rt.o");
        Assert.IsTrue(File.Exists(src), $"math.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    [TestMethod]
    public void Math_AllFunctions()
    {
        string srcFile = Path.Combine(ProgramDir, "math_test.c");
        string elfFile = Path.Combine(TestDir, "math_test.elf");
        Assert.IsTrue(File.Exists(srcFile));
        
        string rtObj = EnsureRuntimeObject();
        string sfObj = EnsureSoftFloatObject();
        string scObj = EnsureSyscallsObject();
        string lcObj = EnsureLibcObject();
        string maObj = EnsureMathObject();
        CompileC(new[] { srcFile, lcObj, maObj, sfObj, scObj, rtObj }, elfFile);
        
        // math on softfloat is slow, give it plenty of steps
        var (output, exitCode, halted) = RunElf(elfFile, 200_000_000);
        Assert.IsTrue(halted, $"Did not halt. Output so far:\n{output}");
        Assert.AreEqual(0, exitCode, $"Exit code {exitCode}. Output:\n{output}");
        
        // All approx checks should print ":1"
        foreach (string line in output.Split('\n'))
        {
            string trimmed = line.Trim();
            if (trimmed.Length > 0 && trimmed.Contains(":") && !trimmed.StartsWith("PASS"))
            {
                Assert.IsTrue(trimmed.EndsWith(":1"),
                    $"Math check failed: {trimmed}\nFull output:\n{output}");
            }
        }
        Assert.IsTrue(output.Contains("PASS"), $"No PASS found. Output:\n{output}");
    }
}
