using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class MemWidthsTest : EmulatorTestBase
{
    // Exact output lines in the order emitted by mem_widths.c.
    // All use check() which formats with put_int (signed decimal).
    private static readonly string[] ExpectedLines =
    {
        "lb 0xFF sign=-1: -1 OK",         // signed byte 0xFF = -1
        "lbu 0xFF zero=255: 255 OK",      // unsigned byte 0xFF = 255
        "lb 0x7F sign=127: 127 OK",       // signed byte 0x7F = 127
        "lh 0xFFFF sign=-1: -1 OK",       // signed halfword 0xFFFF = -1
        "lhu 0xFFFF zero=65535: 65535 OK",// unsigned halfword 0xFFFF = 65535
        "lh 0x8000 sign=-32768: -32768 OK",// signed halfword 0x8000 = -32768
        "lw 0xDEADBEEF: -559038737 OK",   // 0xDEADBEEF as signed int
        "le byte0=4: 4 OK",               // little-endian: 0x01020304 byte[0]=4
        "le byte1=3: 3 OK",               //                           byte[1]=3
        "le byte2=2: 2 OK",               //                           byte[2]=2
        "le byte3=1: 1 OK",               //                           byte[3]=1
        "sh byte0=0x34: 52 OK",           // SH 0x1234: byte[0]=0x34=52
        "sh byte1=0x12: 18 OK",           //             byte[1]=0x12=18
    };

    [TestMethod]
    public void AllLoadStoreWidthsAndSignExtensionAreCorrect()
    {
        string text = RunProgramWithLibc("mem_widths");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(ExpectedLines.Length, lines.Length,
            $"Expected {ExpectedLines.Length} lines, got {lines.Length}.\nFull output:\n{text}");

        for (int i = 0; i < ExpectedLines.Length; i++)
            Assert.AreEqual(ExpectedLines[i], lines[i].Trim(),
                $"Line {i}: expected '{ExpectedLines[i]}', got '{lines[i].Trim()}'");
    }
}
