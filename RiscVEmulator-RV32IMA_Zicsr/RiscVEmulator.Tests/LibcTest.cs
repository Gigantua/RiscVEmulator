using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class LibcTest : EmulatorTestBase
{
    [TestMethod]
    public void Printf_AllFormats()
    {
        // Uses softfloat for %f double arithmetic
        string output = RunProgramWithLibc("printf_test", maxSteps: 50_000_000);

        // Basic strings
        StringAssert.Contains(output, "hello\n");
        StringAssert.Contains(output, "hello world\n");
        StringAssert.Contains(output, "puts test\n");

        // Integers
        StringAssert.Contains(output, "42\n");
        StringAssert.Contains(output, "-123\n");
        StringAssert.Contains(output, "4000000000\n");
        StringAssert.Contains(output, "dead\n");
        StringAssert.Contains(output, "BEEF\n");
        StringAssert.Contains(output, "10\n"); // octal 8

        // Width and padding
        StringAssert.Contains(output, "[   42]\n");
        StringAssert.Contains(output, "[42   ]\n");
        StringAssert.Contains(output, "[00042]\n");

        // Char
        StringAssert.Contains(output, "A\n");

        // Float
        StringAssert.Contains(output, "3.14159");
        StringAssert.Contains(output, "2.72");
        StringAssert.Contains(output, "100\n");  // %.0f of 99.9
        StringAssert.Contains(output, "-1.5");
        StringAssert.Contains(output, "0.0\n");

        // Percent literal
        StringAssert.Contains(output, "100%\n");

        // Mixed — %.4f rounds 3.14159 to 3.1416
        StringAssert.Contains(output, "name=test age=25 pi=3.1416");

        // String precision
        StringAssert.Contains(output, "[abc]\n");

        // NULL
        StringAssert.Contains(output, "(null)\n");

        // Final pass marker
        StringAssert.Contains(output, "PASS\n");
    }
}
