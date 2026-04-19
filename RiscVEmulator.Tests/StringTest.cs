using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class StringTest : EmulatorTestBase
{
    [TestMethod]
    public void String_AllFunctions()
    {
        string output = RunProgramWithLibc("string_test");

        Assert.IsTrue(output.Contains("strlen_empty:0"));
        Assert.IsTrue(output.Contains("strlen_hello:5"));
        Assert.IsTrue(output.Contains("strcmp_eq:1"));
        Assert.IsTrue(output.Contains("strcmp_lt:1"));
        Assert.IsTrue(output.Contains("strcmp_gt:1"));
        Assert.IsTrue(output.Contains("strncmp_eq:1"));
        Assert.IsTrue(output.Contains("strncmp_ne:1"));
        Assert.IsTrue(output.Contains("strcpy:hello"));
        Assert.IsTrue(output.Contains("strncpy:hi"));
        Assert.IsTrue(output.Contains("strncpy_pad:1"));
        Assert.IsTrue(output.Contains("strcat:hello world"));
        Assert.IsTrue(output.Contains("strncat:foobar"));
        Assert.IsTrue(output.Contains("strchr:l"));
        Assert.IsTrue(output.Contains("strchr_null:1"));
        Assert.IsTrue(output.Contains("strrchr:l"));
        Assert.IsTrue(output.Contains("strstr:world"));
        Assert.IsTrue(output.Contains("strstr_null:1"));
        Assert.IsTrue(output.Contains("memcmp_eq:1"));
        Assert.IsTrue(output.Contains("memcmp_ne:1"));
        Assert.IsTrue(output.Contains("memchr:l"));
        Assert.IsTrue(output.Contains("memchr_null:1"));
        Assert.IsTrue(output.Contains("memmove:ababcdef"));
        Assert.IsTrue(output.Contains("PASS"));
    }
}
