using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class AudioTest : EmulatorTestBase
{
    [TestMethod]
    public void AudioBufferAndControlRegisters()
    {
        string srcFile = Path.Combine(ProgramDir, "audio_test.c");
        string elfFile = Path.Combine(TestDir, "audio_test.elf");
        string rtObj = EnsureRuntimeObject();
        CompileC(new[] { srcFile, rtObj }, elfFile);

        var result = RunElfFull(elfFile);

        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        string[] lines = result.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        Assert.AreEqual("audio_test", lines[0]);
        Assert.AreEqual("buf_0: OK", lines[1]);
        Assert.AreEqual("buf_127: OK", lines[2]);
        Assert.AreEqual("buf_128: OK", lines[3]);
        Assert.AreEqual("buf_255: OK", lines[4]);
        Assert.AreEqual("srate: OK", lines[5]);
        Assert.AreEqual("channels: OK", lines[6]);
        Assert.AreEqual("bitdepth: OK", lines[7]);
        Assert.AreEqual("buf_start: OK", lines[8]);
        Assert.AreEqual("buf_len: OK", lines[9]);
        Assert.AreEqual("playing: OK", lines[10]);
        Assert.AreEqual("stopped: OK", lines[11]);
        Assert.AreEqual("audio_done", lines[12]);
        Assert.AreEqual(13, lines.Length);

        // Verify audio buffer contents directly
        var buf = result.AudioBuffer.Buffer;
        for (int i = 0; i < 128; i++)
            Assert.AreEqual(0x7F, buf[i], $"AudioBuffer[{i}] should be 0x7F");
        for (int i = 128; i < 256; i++)
            Assert.AreEqual(0x80, buf[i], $"AudioBuffer[{i}] should be 0x80");

        // Verify control state after stop
        Assert.IsFalse(result.AudioControl.IsPlaying);
        Assert.AreEqual(44100u, result.AudioControl.SampleRate);
        Assert.AreEqual(2u, result.AudioControl.Channels);
        Assert.AreEqual(16u, result.AudioControl.BitDepth);
    }
}
