using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

[TestClass]
public class InputTest : EmulatorTestBase
{
    [TestMethod]
    public void KeyboardAndMouseEventsAreCorrect()
    {
        string srcFile = Path.Combine(ProgramDir, "input_test.c");
        string elfFile = Path.Combine(TestDir, "input_test.elf");
        string rtObj = EnsureRuntimeObject();
        CompileC(new[] { srcFile, rtObj }, elfFile);

        var result = RunElfFull(elfFile,
            setupKeyboard: kbd =>
            {
                kbd.EnqueueKey(0x1E, true);   // A pressed
                kbd.EnqueueKey(0x30, true);   // B pressed
                kbd.EnqueueKey(0x1E, false);  // A released
            },
            setupMouse: mouse =>
            {
                mouse.MoveMouse(10, -5);
                mouse.SetButton(0, true);  // left button
            });

        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        string[] lines = result.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        Assert.AreEqual("input_test", lines[0]);
        Assert.AreEqual("kbd_has_data: OK", lines[1]);
        Assert.AreEqual("key1=286", lines[2]);  // 0x11E = 286
        Assert.AreEqual("key1_A_press: OK", lines[3]);
        Assert.AreEqual("key2_B_press: OK", lines[4]);
        Assert.AreEqual("key3_A_release: OK", lines[5]);
        Assert.AreEqual("kbd_empty: OK", lines[6]);
        Assert.AreEqual("mouse_has_data: OK", lines[7]);
        Assert.AreEqual("dx=10", lines[8]);
        Assert.AreEqual("dy=-5", lines[9]);
        Assert.AreEqual("btn=1", lines[10]);
        Assert.AreEqual("mouse_dx_10: OK", lines[11]);
        Assert.AreEqual("mouse_dy_neg5: OK", lines[12]);
        Assert.AreEqual("mouse_left_btn: OK", lines[13]);
        Assert.AreEqual("mouse_dx_reset: OK", lines[14]);
        Assert.AreEqual(15, lines.Length);
    }
}
