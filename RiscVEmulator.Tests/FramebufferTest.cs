using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class FramebufferTest : EmulatorTestBase
{
    [TestMethod]
    public void FramebufferPixelsAreCorrect()
    {
        string srcFile = Path.Combine(ProgramDir, "fb_test.c");
        string elfFile = Path.Combine(TestDir, "fb_test.elf");
        string rtObj = EnsureRuntimeObject();
        CompileC(new[] { srcFile, rtObj }, elfFile);

        var result = RunElfFull(elfFile);
        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        string[] lines = result.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        Assert.AreEqual("fb_test", lines[0]);
        Assert.AreEqual("width=320", lines[1]);
        Assert.AreEqual("height=200", lines[2]);
        Assert.AreEqual("bpp=32", lines[3]);
        Assert.AreEqual("width_320: OK", lines[4]);
        Assert.AreEqual("height_200: OK", lines[5]);
        Assert.AreEqual("bpp_32: OK", lines[6]);
        Assert.AreEqual("pixels_written: OK", lines[7]);

        byte[] px = result.Framebuffer.Pixels;

        // Pixel (0,0) = red RGBA
        Assert.AreEqual(0xFF, px[0], "R of (0,0)");
        Assert.AreEqual(0x00, px[1], "G of (0,0)");
        Assert.AreEqual(0x00, px[2], "B of (0,0)");
        Assert.AreEqual(0xFF, px[3], "A of (0,0)");

        // Pixel (1,0) = green RGBA
        Assert.AreEqual(0x00, px[4], "R of (1,0)");
        Assert.AreEqual(0xFF, px[5], "G of (1,0)");
        Assert.AreEqual(0x00, px[6], "B of (1,0)");
        Assert.AreEqual(0xFF, px[7], "A of (1,0)");

        // Pixel (0,1) = blue RGBA at row offset 1280
        int row1 = 320 * 4;
        Assert.AreEqual(0x00, px[row1 + 0], "R of (0,1)");
        Assert.AreEqual(0x00, px[row1 + 1], "G of (0,1)");
        Assert.AreEqual(0xFF, px[row1 + 2], "B of (0,1)");
        Assert.AreEqual(0xFF, px[row1 + 3], "A of (0,1)");

        // Pixel (2,0) = white RGBA (word-write 0xFFFFFFFF)
        Assert.AreEqual(0xFF, px[8],  "R of (2,0)");
        Assert.AreEqual(0xFF, px[9],  "G of (2,0)");
        Assert.AreEqual(0xFF, px[10], "B of (2,0)");
        Assert.AreEqual(0xFF, px[11], "A of (2,0)");

        // Vsync flag
        Assert.AreEqual(1u, result.DisplayControl.VsyncFlag, "vsync flag");
    }
}
