using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class SignedOpsTest : EmulatorTestBase
{
    // Exact output lines in the order emitted by signed_ops.c.
    // check_i() -> put_int  (signed decimal)
    // check_u() -> put_uint (unsigned decimal, NOT hex)
    private static readonly string[] ExpectedLines =
    {
        // Signed DIV
        "7 div 3: 2 OK",
        "7 div -1: -7 OK",
        "-7 div 3: -2 OK",
        "-7 div -1: 7 OK",
        // Signed REM
        "7 rem 3: 1 OK",
        "-7 rem 3: -1 OK",
        "7 rem -1: 0 OK",
        "-7 rem -1: 0 OK",
        // M-extension edge cases
        "div/0 = -1: -1 OK",               // signed div-by-zero -> -1
        "divu/0 = UMAX: 4294967295 OK",    // unsigned div-by-zero -> 0xFFFFFFFF (decimal)
        "rem/0 = dividend: 7 OK",          // rem-by-zero -> dividend
        "remu/0 = dividend: 7 OK",         // remu-by-zero -> dividend
        "INT_MIN/-1=INT_MIN: -2147483648 OK",
        // MULH variants (upper 32 bits of 64-bit product)
        "mulh(INT_MIN,INT_MIN): 1073741824 OK",  // (-2^31)^2>>32 = 0x40000000
        "mulhu(UMAX,UMAX): 4294967294 OK",        // 0xFFFFFFFF^2 upper = 0xFFFFFFFE
        "mulhsu(-1,2): 4294967295 OK",             // signed(-1)*unsigned(2) upper = 0xFFFFFFFF
        // SLT vs SLTU
        "slt -1<0: 1 OK",
        "sltu -1<0u: 0 OK",
        "slt 0<-1: 0 OK",
        // Signed branches
        "blt -1<1: 1 OK",
        "bge 1>=-1: 1 OK",
        // SRAI on negative
        "srai -8>>1=-4: -4 OK",
        "srai -8>>31=-1: -1 OK",
    };

    [TestMethod]
    public void SignedArithmeticAndMExtEdgeCasesAreCorrect()
    {
        string text = RunProgramWithLibc("signed_ops");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
