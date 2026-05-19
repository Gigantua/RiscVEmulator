using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Comprehensive MMIO peripheral tests. Compiles mmio_devices_test.c which
/// exercises all memory-mapped devices from the guest side, then verifies
/// both the guest-reported results (via UART output) and the actual
/// peripheral state from the host side.
/// </summary>
[TestClass]
public class MmioDevicesTest : EmulatorTestBase
{
    private static RunResult _result = null!;
    private static string _output = "";

    [ClassInitialize]
    public static void Build(TestContext ctx)
    {
        string rtObj = EnsureRuntimeObject();
        string srcFile = Path.Combine(ProgramDir, "mmio_devices_test.c");
        string elfFile = Path.Combine(TestDir, "mmio_devices_test.elf");
        CompileC(new[] { srcFile, rtObj }, elfFile);

        _result = RunElfFull(elfFile, 20_000_000,
            setupKeyboard: kbd =>
            {
                // WASD press + W release, with shift modifier
                kbd.EnqueueKey(0x77, true);   // 'w' press
                kbd.EnqueueKey(0x61, true);   // 'a' press
                kbd.EnqueueKey(0x73, true);   // 's' press
                kbd.EnqueueKey(0x64, true);   // 'd' press
                kbd.EnqueueKey(0x77, false);  // 'w' release
                kbd.SetModifiers(shift: true, ctrl: false, alt: false);
            },
            setupMouse: mouse =>
            {
                mouse.MoveMouse(42, -17);
                mouse.SetButton(0, true);   // left
                mouse.SetButton(1, true);   // right
            },
            setupUart: uart =>
            {
                // Inject "Hi!" into UART RX FIFO
                uart.EnqueueInput((byte)'H');
                uart.EnqueueInput((byte)'i');
                uart.EnqueueInput((byte)'!');
            });

        Assert.IsTrue(_result.Halted, "emulator did not halt");
        Assert.AreEqual(0, _result.ExitCode, $"exit code {_result.ExitCode}");
        _output = _result.Output;
    }

    // Helper to check guest-side OK labels
    private static void AssertOk(string label)
        => StringAssert.Contains(_output, $"{label}: OK", $"'{label}' not found as OK in output:\n{_output}");

    // ── UART ──────────────────────────────────────────────────

    [TestMethod] public void UartTxReady()         => AssertOk("uart_tx_ready");
    [TestMethod] public void UartRxHasData()      => AssertOk("uart_rx_has_data");

    [TestMethod]
    public void UartOutputContainsBang()
    {
        // The guest writes '!' via UART_THR — should appear in output
        Assert.IsTrue(_output.Contains("!"), "UART TX output should contain '!'");
    }

    // ── Keyboard ──────────────────────────────────────────────

    [TestMethod] public void KbdHasEvents()    => AssertOk("kbd_has_events");
    [TestMethod] public void KbdWPress()       => AssertOk("kbd_w_press");
    [TestMethod] public void KbdAPress()       => AssertOk("kbd_a_press");
    [TestMethod] public void KbdSPress()       => AssertOk("kbd_s_press");
    [TestMethod] public void KbdDPress()       => AssertOk("kbd_d_press");
    [TestMethod] public void KbdWRelease()     => AssertOk("kbd_w_release");
    [TestMethod] public void KbdFifoEmpty()    => AssertOk("kbd_fifo_empty");
    [TestMethod] public void KbdEmptyReadZero() => AssertOk("kbd_empty_read_zero");
    [TestMethod] public void KbdModShift()     => AssertOk("kbd_mod_shift");
    [TestMethod] public void KbdModNoCtrl()    => AssertOk("kbd_mod_no_ctrl");
    [TestMethod] public void KbdModNoAlt()     => AssertOk("kbd_mod_no_alt");

    // ── Mouse ─────────────────────────────────────────────────

    [TestMethod] public void MouseHasData()    => AssertOk("mouse_has_data");
    [TestMethod] public void MouseDx42()       => AssertOk("mouse_dx_42");
    [TestMethod] public void MouseDyNeg17()    => AssertOk("mouse_dy_neg17");
    [TestMethod] public void MouseLeftBtn()    => AssertOk("mouse_left_btn");
    [TestMethod] public void MouseRightBtn()   => AssertOk("mouse_right_btn");
    [TestMethod] public void MouseNoMiddle()   => AssertOk("mouse_no_middle");
    [TestMethod] public void MouseDxReset()    => AssertOk("mouse_dx_reset");
    [TestMethod] public void MouseDyReset()    => AssertOk("mouse_dy_reset");
    [TestMethod] public void MouseBtnPersists() => AssertOk("mouse_btn_persists");
    [TestMethod] public void MouseStatusBtn()  => AssertOk("mouse_status_btn");

    // ── Real-Time Clock ───────────────────────────────────────

    [TestMethod] public void RtcUsNonzero()      => AssertOk("rtc_us_nonzero");
    [TestMethod] public void RtcUsAdvances()      => AssertOk("rtc_us_advances");
    [TestMethod] public void RtcMsReadable()      => AssertOk("rtc_ms_readable");
    [TestMethod] public void RtcEpochPlausible()  => AssertOk("rtc_epoch_plausible");
    [TestMethod] public void RtcEpochHiZero()     => AssertOk("rtc_epoch_hi_zero");
    [TestMethod] public void RtcVsClintDiffer()   => AssertOk("rtc_vs_clint_differ");

    // ── Framebuffer ───────────────────────────────────────────

    [TestMethod] public void FbWidth320()         => AssertOk("fb_width_320");
    [TestMethod] public void FbHeight200()        => AssertOk("fb_height_200");
    [TestMethod] public void FbYellowWritten()    => AssertOk("fb_yellow_written");
    [TestMethod] public void FbMagentaWord()      => AssertOk("fb_magenta_word");
    [TestMethod] public void FbReadbackR()        => AssertOk("fb_readback_r");
    [TestMethod] public void FbReadbackG()        => AssertOk("fb_readback_g");
    [TestMethod] public void FbReadbackB()        => AssertOk("fb_readback_b");
    [TestMethod] public void FbReadbackA()        => AssertOk("fb_readback_a");
    [TestMethod] public void FbLastPixel()        => AssertOk("fb_last_pixel");

    /// <summary>Verify yellow pixels are actually in the framebuffer from the host side.</summary>
    [TestMethod]
    public void FbYellowPixelsVerifiedFromHost()
    {
        byte[] px = _result.Framebuffer.Pixels;
        for (int x = 100; x < 110; x++)
        {
            int off = (50 * 320 + x) * 4;
            Assert.AreEqual(0xFF, px[off + 0], $"Yellow pixel ({x},50) R");
            Assert.AreEqual(0xFF, px[off + 1], $"Yellow pixel ({x},50) G");
            Assert.AreEqual(0x00, px[off + 2], $"Yellow pixel ({x},50) B");
            Assert.AreEqual(0xFF, px[off + 3], $"Yellow pixel ({x},50) A");
        }
    }

    /// <summary>Verify magenta pixel at (0,0) from host side.</summary>
    [TestMethod]
    public void FbMagentaPixelVerifiedFromHost()
    {
        byte[] px = _result.Framebuffer.Pixels;
        Assert.AreEqual(0xFF, px[0], "Magenta (0,0) R");
        Assert.AreEqual(0x00, px[1], "Magenta (0,0) G");
        Assert.AreEqual(0xFF, px[2], "Magenta (0,0) B");
        Assert.AreEqual(0xFF, px[3], "Magenta (0,0) A");
    }

    /// <summary>Verify last pixel (319,199) = gray from host side.</summary>
    [TestMethod]
    public void FbLastPixelVerifiedFromHost()
    {
        byte[] px = _result.Framebuffer.Pixels;
        int off = (199 * 320 + 319) * 4;
        Assert.AreEqual(0x80, px[off + 0], "Last pixel R");
        Assert.AreEqual(0x80, px[off + 1], "Last pixel G");
        Assert.AreEqual(0x80, px[off + 2], "Last pixel B");
        Assert.AreEqual(0xFF, px[off + 3], "Last pixel A");
    }

    /// <summary>Unwritten pixels should still be zero (black).</summary>
    [TestMethod]
    public void FbUnwrittenPixelsAreBlack()
    {
        byte[] px = _result.Framebuffer.Pixels;
        // Check pixel (160, 100) — middle of screen, not written by test
        int off = (100 * 320 + 160) * 4;
        Assert.AreEqual(0, px[off + 0], "Unwritten R");
        Assert.AreEqual(0, px[off + 1], "Unwritten G");
        Assert.AreEqual(0, px[off + 2], "Unwritten B");
        Assert.AreEqual(0, px[off + 3], "Unwritten A");
    }

    // ── Display Control ───────────────────────────────────────

    [TestMethod] public void DispPalEntriesSet() => AssertOk("pal_entries_set");
    [TestMethod] public void DispModeIndexed()   => AssertOk("mode_indexed");
    [TestMethod] public void DispModeDirect()    => AssertOk("mode_direct");
    [TestMethod] public void DispVsyncSet()      => AssertOk("vsync_set");

    /// <summary>Verify palette entries from host side.</summary>
    [TestMethod]
    public void DispPaletteVerifiedFromHost()
    {
        var palette = _result.DisplayControl.Palette;
        Assert.AreEqual(0x00FF0000u, palette[0],   "Palette[0] should be red");
        Assert.AreEqual(0x000000AAu, palette[42],  "Palette[42] should be dark blue");
        Assert.AreEqual(0x00FFFFFFu, palette[255], "Palette[255] should be white");
    }

    /// <summary>Vsync flag should be 1 after guest set it.</summary>
    [TestMethod]
    public void DispVsyncFlagFromHost()
    {
        Assert.AreEqual(1u, _result.DisplayControl.VsyncFlag);
    }

    /// <summary>Mode should be 0 (direct) — guest switched back from indexed.</summary>
    [TestMethod]
    public void DispModeIsDirectFromHost()
    {
        Assert.AreEqual(0u, _result.DisplayControl.Mode);
    }

    // ── Audio ─────────────────────────────────────────────────

    [TestMethod] public void AudioBufWritten()  => AssertOk("audio_buf_written");
    [TestMethod] public void AudioBufRead0()    => AssertOk("audio_buf_read_0");
    [TestMethod] public void AudioBufRead511()  => AssertOk("audio_buf_read_511");
    [TestMethod] public void AudioBufRead512()  => AssertOk("audio_buf_read_512");
    [TestMethod] public void AudioBufRead1023() => AssertOk("audio_buf_read_1023");
    [TestMethod] public void AudioSrate()       => AssertOk("audio_srate");
    [TestMethod] public void AudioChan()        => AssertOk("audio_chan");
    [TestMethod] public void AudioBits()        => AssertOk("audio_bits");
    [TestMethod] public void AudioBstart()      => AssertOk("audio_bstart");
    [TestMethod] public void AudioBlen()        => AssertOk("audio_blen");
    [TestMethod] public void AudioPlaying()     => AssertOk("audio_playing");
    [TestMethod] public void AudioStopped()     => AssertOk("audio_stopped");

    /// <summary>Verify audio buffer contents from host side.</summary>
    [TestMethod]
    public void AudioBufferVerifiedFromHost()
    {
        var buf = _result.AudioBuffer.Buffer;
        for (int i = 0; i < 512; i++)
            Assert.AreEqual(0xAA, buf[i], $"AudioBuf[{i}] should be 0xAA");
        for (int i = 512; i < 1024; i++)
            Assert.AreEqual(0x55, buf[i], $"AudioBuf[{i}] should be 0x55");
    }

    /// <summary>Verify audio control state from host after stop.</summary>
    [TestMethod]
    public void AudioControlStateFromHost()
    {
        Assert.IsFalse(_result.AudioControl.IsPlaying, "Should be stopped");
        Assert.AreEqual(11025u, _result.AudioControl.SampleRate);
        Assert.AreEqual(2u, _result.AudioControl.Channels);
        Assert.AreEqual(16u, _result.AudioControl.BitDepth);
    }

    // ── UART RX ───────────────────────────────────────────────

    [TestMethod] public void UartRxReady()  => AssertOk("uart_rx_ready");
    [TestMethod] public void UartRxH()      => AssertOk("uart_rx_H");
    [TestMethod] public void UartRxI()      => AssertOk("uart_rx_i");
    [TestMethod] public void UartRxBang()   => AssertOk("uart_rx_bang");
    [TestMethod] public void UartRxEmpty()  => AssertOk("uart_rx_empty");

    // ── Completion ────────────────────────────────────────────

    [TestMethod]
    public void CompletionMessage()
        => StringAssert.Contains(_output, "mmio_devices_test: done");
}
