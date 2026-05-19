using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class BubbleSortTest : EmulatorTestBase
{
    // Original array from bubblesort.c — order must match exactly in "Before" line
    private static readonly int[] Original =
        { 64, -3, 17, 0, 99, -50, 42, 7, 7, 100, -1, 28, 55, 13, -99, 6, 88, 33, 21, 0 };

    // Same elements sorted ascending — order must match exactly in "After" line
    private static readonly int[] Sorted =
        { -99, -50, -3, -1, 0, 0, 6, 7, 7, 13, 17, 21, 28, 33, 42, 55, 64, 88, 99, 100 };

    [TestMethod]
    public void ArrayIsSortedCorrectly()
    {
        string text = RunProgramWithLibc("bubblesort");
        string[] lines = text.Split('\n', System.StringSplitOptions.RemoveEmptyEntries);

        Assert.AreEqual(2, lines.Length,
            $"Expected exactly 2 output lines (Before + After), got {lines.Length}.\nFull output:\n{text}");

        // ── Before line ──────────────────────────────────────────
        Assert.IsTrue(lines[0].StartsWith("Before: "),
            $"Line 0 should start with 'Before: ', got: '{lines[0]}'");

        string[] beforeParts = lines[0].Replace("Before: ", "")
                                       .Split(' ', System.StringSplitOptions.RemoveEmptyEntries);
        Assert.AreEqual(20, beforeParts.Length,
            $"Before line should contain 20 numbers, got {beforeParts.Length}");

        for (int i = 0; i < 20; i++)
        {
            Assert.IsTrue(int.TryParse(beforeParts[i], out int v),
                $"Before[{i}]: could not parse '{beforeParts[i]}'");
            Assert.AreEqual(Original[i], v,
                $"Before[{i}]: expected {Original[i]}, got {v}");
        }

        // ── After line ───────────────────────────────────────────
        Assert.IsTrue(lines[1].StartsWith("After:  "),
            $"Line 1 should start with 'After:  ' (two spaces), got: '{lines[1]}'");

        string[] afterParts = lines[1].Replace("After:  ", "")
                                      .Split(' ', System.StringSplitOptions.RemoveEmptyEntries);
        Assert.AreEqual(20, afterParts.Length,
            $"After line should contain 20 numbers, got {afterParts.Length}");

        for (int i = 0; i < 20; i++)
        {
            Assert.IsTrue(int.TryParse(afterParts[i], out int v),
                $"After[{i}]: could not parse '{afterParts[i]}'");
            Assert.AreEqual(Sorted[i], v,
                $"After[{i}]: expected {Sorted[i]}, got {v}");
        }
    }
}
