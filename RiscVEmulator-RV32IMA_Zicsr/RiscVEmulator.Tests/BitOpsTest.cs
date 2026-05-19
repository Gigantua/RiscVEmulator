using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class BitOpsTest : EmulatorTestBase
{
    // Exact output lines in the order emitted by bitops.c.
    // check()   -> decimal signed   e.g. "name: -4 OK"
    // check_u() -> 0x-prefixed hex  e.g. "name: 0x10000000 OK"
    private static readonly string[] ExpectedLines =
    {
        "slli 1<<4: 16 OK",                        // check  (decimal)
        "srli >>3: 0x10000000 OK",                 // check_u (hex)
        "srai neg>>1: -4 OK",                      // check  (decimal)
        "srai neg>>31: -1 OK",                     // check  (decimal)
        "slli by 0: -8 OK",                        // check  (decimal, -8 << 0 = -8)
        "srli by 31: 0x00000001 OK",               // check_u (hex)
        "sll  reg: 0x00000008 OK",                 // check_u (hex, 1 << 3 = 8)
        "srl  reg: 0x00000010 OK",                 // check_u (hex, 0x80 >> 3 = 0x10)
        "sra  reg neg: -268435456 OK",             // check  (decimal, 0x80000000>>3 signed)
        "srl  by 31: 0x00000001 OK",               // check_u (hex)
        "sra  by 31: -1 OK",                       // check  (decimal)
        "xor: 0xffffffff OK",                      // check_u
        "xori: 0xffffff00 OK",                     // check_u
        "or: 0xffffffff OK",                       // check_u
        "ori: 0x000000ab OK",                      // check_u
        "and: 0x00ffff00 OK",                      // check_u
        "andi: 0x0000000f OK",                     // check_u
        "popcount(0xF0F0F0F0): 16 OK",             // check  (decimal)
        "popcount(0): 0 OK",                       // check  (decimal)
        "rotl32(1,4): 0x00000010 OK",              // check_u (hex, 1<<4=16=0x10)
        "rotl32(0x80000000,1): 0x00000001 OK",     // check_u (hex, rotated MSB)
    };

    [TestMethod]
    public void AllBitOperationsProduceCorrectResults()
    {
        string text = RunProgramWithLibc("bitops");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
