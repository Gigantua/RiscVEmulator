using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class CtypeTest : EmulatorTestBase
{
    [TestMethod]
    public void Ctype_AllFunctions()
    {
        string output = RunProgramWithLibc("ctype_test");

        Assert.IsTrue(output.Contains("isdigit_0:1"), $"Failed isdigit_0. Output:\n{output}");
        Assert.IsTrue(output.Contains("isdigit_9:1"));
        Assert.IsTrue(output.Contains("isdigit_a:1"));
        Assert.IsTrue(output.Contains("isalpha_A:1"));
        Assert.IsTrue(output.Contains("isalpha_z:1"));
        Assert.IsTrue(output.Contains("isalpha_5:1"));
        Assert.IsTrue(output.Contains("isalnum_a:1"));
        Assert.IsTrue(output.Contains("isalnum_5:1"));
        Assert.IsTrue(output.Contains("isalnum_bang:1"));
        Assert.IsTrue(output.Contains("isspace_sp:1"));
        Assert.IsTrue(output.Contains("isspace_tab:1"));
        Assert.IsTrue(output.Contains("isspace_a:1"));
        Assert.IsTrue(output.Contains("isupper_A:1"));
        Assert.IsTrue(output.Contains("isupper_a:1"));
        Assert.IsTrue(output.Contains("islower_a:1"));
        Assert.IsTrue(output.Contains("islower_A:1"));
        Assert.IsTrue(output.Contains("toupper_a:A"));
        Assert.IsTrue(output.Contains("tolower_A:a"));
        Assert.IsTrue(output.Contains("toupper_5:5"));
        Assert.IsTrue(output.Contains("isxdigit_f:1"));
        Assert.IsTrue(output.Contains("isxdigit_F:1"));
        Assert.IsTrue(output.Contains("isxdigit_g:1"));
        Assert.IsTrue(output.Contains("isprint_a:1"));
        Assert.IsTrue(output.Contains("isprint_nul:1"));
        Assert.IsTrue(output.Contains("iscntrl_nul:1"));
        Assert.IsTrue(output.Contains("iscntrl_a:1"));
        Assert.IsTrue(output.Contains("PASS"));
    }
}
