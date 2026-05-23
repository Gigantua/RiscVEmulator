using System.Diagnostics;
using System.Text.RegularExpressions;

namespace RiscVEmulator.Core;

// One place that knows where the project's clang lives: the self-built
// LLVM in WSL at ~/llvm-rv32i/. Examples and Linux.Build_RV32i go
// through here so a single patched clang covers the whole stack.
//
// Override with env var RVEMU_CLANG (a WSL path, e.g.
// "/usr/bin/clang-18") to point at a different toolchain.
public static class WslClang
{
    // RISC5 Phase 2: the patched clang (with FeatureXArx + the
    // RISCVReduceToFiveOpsPass) lives at ~/llvm-build/bin/clang.
    // Override via RVEMU_CLANG when needed.
    public const string DefaultClangPath = "/home/daniel/llvm-build/bin/clang";

    public static string ClangPath =>
        Environment.GetEnvironmentVariable("RVEMU_CLANG") is { Length: > 0 } v
            ? v : DefaultClangPath;

    // Spawn wsl.exe <clang> <args...>. Returns true on exit 0. On
    // failure, prints stderr and the failed command line to Console.Error.
    // Windows paths inside args are translated to /mnt/<drive>/... form.
    public static bool Run(IEnumerable<string> args, string? cwd = null)
        => Run(args, out _, cwd);

    public static bool Run(IEnumerable<string> args, out string stderr, string? cwd = null)
    {
        var translated = args.Select(TranslatePathsInArg).ToList();
        var psi = new ProcessStartInfo("wsl.exe")
        {
            RedirectStandardError  = true,
            RedirectStandardOutput = true,
            UseShellExecute        = false,
        };
        if (cwd != null) psi.WorkingDirectory = cwd;

        psi.ArgumentList.Add("--");
        psi.ArgumentList.Add(ClangPath);
        foreach (var a in translated) psi.ArgumentList.Add(a);

        using var proc = Process.Start(psi)!;
        stderr = proc.StandardError.ReadToEnd();
        string stdout = proc.StandardOutput.ReadToEnd();
        proc.WaitForExit();

        if (proc.ExitCode != 0)
        {
            Console.Error.WriteLine($"FAILED (exit {proc.ExitCode})");
            Console.Error.WriteLine($"  wsl -- {ClangPath} {string.Join(' ', translated)}");
            if (stdout.Length > 0) Console.Error.Write(stdout);
            if (stderr.Length > 0) Console.Error.Write(stderr);
            return false;
        }
        return true;
    }

    // C:\foo\bar  -> /mnt/c/foo/bar     (also handles forward slashes)
    // /mnt/...    -> unchanged
    public static string ToWslPath(string winPath)
    {
        if (winPath.Length >= 2 && winPath[1] == ':')
        {
            char drive = char.ToLowerInvariant(winPath[0]);
            string rest = winPath.Substring(2).Replace('\\', '/');
            if (rest.Length > 0 && rest[0] != '/') rest = "/" + rest;
            return $"/mnt/{drive}{rest}";
        }
        return winPath.Replace('\\', '/');
    }

    // Find every "X:\..." or "X:/..." substring inside an argument and
    // rewrite it. Catches plain paths and flag forms like -IC:\foo,
    // -o, -Wl,-T,C:\path, --sysroot=C:\..., etc.
    private static readonly Regex WinPathRx = new(
        @"[A-Za-z]:[\\/][^\s""':;,]*",
        RegexOptions.Compiled);

    public static string TranslatePathsInArg(string arg)
        => WinPathRx.Replace(arg, m => ToWslPath(m.Value));
}
