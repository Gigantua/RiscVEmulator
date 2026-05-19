using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class StdlibTest : EmulatorTestBase
{
    [TestMethod]
    public void Stdlib_AllFunctions()
    {
        string output = RunProgramWithLibc("stdlib_test");
        
        Assert.IsTrue(output.Contains("abs_pos:42"), $"Output:\n{output}");
        Assert.IsTrue(output.Contains("abs_neg:42"));
        Assert.IsTrue(output.Contains("labs_neg:100"));
        Assert.IsTrue(output.Contains("atoi:12345"));
        Assert.IsTrue(output.Contains("atoi_neg:-99"));
        Assert.IsTrue(output.Contains("atoi_space:42"));
        Assert.IsTrue(output.Contains("atol:999999"));
        Assert.IsTrue(output.Contains("strtol_dec:12345"));
        Assert.IsTrue(output.Contains("strtol_hex:255"));
        Assert.IsTrue(output.Contains("strtol_hex0x:255"));
        Assert.IsTrue(output.Contains("strtol_oct:63"));
        Assert.IsTrue(output.Contains("strtol_neg:-42"));
        Assert.IsTrue(output.Contains("strtol_end:abc"));
        // deadbeef = 3735928559
        Assert.IsTrue(output.Contains("strtoul_hex:3735928559"));
        Assert.IsTrue(output.Contains("div_quot:3"));
        Assert.IsTrue(output.Contains("div_rem:2"));
        Assert.IsTrue(output.Contains("ldiv_quot:-14"));
        Assert.IsTrue(output.Contains("ldiv_rem:-2"));
        Assert.IsTrue(output.Contains("rand_diff:1"));
        Assert.IsTrue(output.Contains("rand_repeat:1"));
        Assert.IsTrue(output.Contains("rand_range:1"));
        Assert.IsTrue(output.Contains("qsort:1,2,3,4,5,6,7,8"));
        Assert.IsTrue(output.Contains("bsearch:1"));
        Assert.IsTrue(output.Contains("bsearch_miss:1"));
        Assert.IsTrue(output.Contains("PASS"));
    }
}
