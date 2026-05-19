namespace RiscVEmulator.Core;

/// <summary>Enables ANSI/VT-100 escape sequence processing on Windows stdout.</summary>
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

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern nint GetStdHandle(int nStdHandle);

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool GetConsoleMode(nint hHandle, out uint lpMode);

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool SetConsoleMode(nint hHandle, uint dwMode);
}
