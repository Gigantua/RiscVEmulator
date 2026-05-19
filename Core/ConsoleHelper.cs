namespace RiscVEmulator.Core;

/// <summary>Enables ANSI/VT-100 escape sequence processing on Windows stdout
/// and raw (pass-through) mode on Windows stdin for an interactive serial console.</summary>
public static class ConsoleHelper
{
    public static void EnableVt()
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            // STD_OUTPUT_HANDLE = -11; ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
            var h = GetStdHandle(-11);
            if (GetConsoleMode(h, out uint mode))
                SetConsoleMode(h, mode | 0x0004u);
        }
        catch { /* non-console host (piped / redirected) — ignore */ }
    }

    // Saved original stdin console mode so the terminal can be restored on exit.
    // null = nothing was saved (not Windows, non-console host, or GetConsoleMode failed).
    private static uint? _savedInputMode;

    // NOTE: these are *console input* mode flags. They share numeric values with the
    // *console output* flags (e.g. input ENABLE_PROCESSED_INPUT 0x0001 vs output
    // ENABLE_PROCESSED_OUTPUT 0x0001, and input ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
    // vs output ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004) but they are a completely
    // separate flag namespace that only applies to the STD_INPUT_HANDLE.
    private const uint ENABLE_PROCESSED_INPUT        = 0x0001; // Ctrl+C handled by console
    private const uint ENABLE_LINE_INPUT             = 0x0002; // line buffering
    private const uint ENABLE_ECHO_INPUT             = 0x0004; // console echoes keystrokes
    private const uint ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200; // keys delivered as ANSI escapes

    /// <summary>
    /// Puts the Windows console *input* handle into raw pass-through mode for an
    /// interactive serial console: echo OFF (the guest tty echoes), line-input OFF
    /// (char-at-a-time), processed-input OFF (Ctrl+C passes through as byte 0x03),
    /// virtual-terminal-input ON (arrow/function keys arrive as ANSI escape bytes).
    /// The original mode is saved so <see cref="RestoreInput"/> can undo this.
    /// </summary>
    public static void EnableRawInput()
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            // STD_INPUT_HANDLE = -10
            var h = GetStdHandle(-10);
            if (GetConsoleMode(h, out uint mode))
            {
                _savedInputMode = mode;
                uint raw = (mode & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
                           | ENABLE_VIRTUAL_TERMINAL_INPUT;
                SetConsoleMode(h, raw);
            }
        }
        catch { /* non-console host (piped / redirected stdin) — ignore */ }
    }

    /// <summary>
    /// Restores the console input mode saved by <see cref="EnableRawInput"/> so the
    /// user's terminal is not left in raw mode after the emulator exits.
    /// No-op if nothing was saved (not Windows, non-console host, or save failed).
    /// </summary>
    public static void RestoreInput()
    {
        if (!OperatingSystem.IsWindows()) return;
        if (_savedInputMode is not uint saved) return;
        try
        {
            // STD_INPUT_HANDLE = -10
            var h = GetStdHandle(-10);
            SetConsoleMode(h, saved);
        }
        catch { /* non-console host — ignore */ }
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern nint GetStdHandle(int nStdHandle);

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool GetConsoleMode(nint hHandle, out uint lpMode);

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool SetConsoleMode(nint hHandle, uint dwMode);
}
