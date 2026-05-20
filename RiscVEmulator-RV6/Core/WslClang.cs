// Bridges Windows .NET callers to the WSL-built clang at
// ~/llvm-rv32i/bin/clang (produced by build-llvm.sh in the repo root). Args
// that look like Windows paths (drive-letter or UNC) are translated to
// /mnt/<drive>/... so guest sources, headers, linker scripts and output
// files all resolve inside WSL.
//
// Used by Examples/* and RiscVEmulator.Tests so the full repo builds with
// the self-built RV32I clang. Falls back to system clang on Windows only if
// WSL or the self-built tree is unavailable (set RVEMU_CLANG=fallback).

namespace RiscVEmulator.Core;

using System.Diagnostics;
using System.Text;

public static class WslClang
{
    // Default location produced by build-llvm.sh; override with RVEMU_LLVM_BIN
    // to point at a different prefix (e.g. /opt/llvm-rv32i/bin).
    public static string WslLlvmBin =>
        Environment.GetEnvironmentVariable("RVEMU_LLVM_BIN") ?? "/home/daniel/llvm-rv32i/bin";
    public static string WslClangPath => $"{WslLlvmBin}/clang";

    public static int Run(IEnumerable<string> args, out string stderr,
                          bool inheritOutput = false)
    {
        var psi = new ProcessStartInfo("wsl.exe")
        {
            UseShellExecute        = false,
            RedirectStandardError  = true,
            RedirectStandardOutput = !inheritOutput,
        };
        psi.ArgumentList.Add("--");
        psi.ArgumentList.Add(WslClangPath);
        // -B makes clang search this dir for sub-tools (`ld.lld`), so
        // `-fuse-ld=lld` and `--ld-path=ld.lld` resolve to our self-built
        // linker without needing PATH munging.
        psi.ArgumentList.Add($"-B{WslLlvmBin}");
        foreach (var a in args)
            psi.ArgumentList.Add(Translate(a));

        using var p = Process.Start(psi)!;
        if (!inheritOutput) _ = p.StandardOutput.ReadToEnd();
        stderr = p.StandardError.ReadToEnd();
        p.WaitForExit();
        return p.ExitCode;
    }

    // Translate every `<letter>:[/\]...` segment in `arg` to /mnt/<letter>/...
    // and flip backslashes to forward slashes inside that segment. Non-path
    // text (flag prefixes, commas, equals signs, the rest of the arg)
    // passes through untouched. Handles -IC:\foo, --sysroot=C:\sr,
    // -Wl,-T,C:\linker.ld,--gc-sections, and bare paths.
    public static string Translate(string arg)
    {
        var sb = new StringBuilder(arg.Length + 8);
        int i = 0;
        while (i < arg.Length)
        {
            if (i + 2 < arg.Length
                && char.IsLetter(arg[i])
                && arg[i + 1] == ':'
                && (arg[i + 2] == '\\' || arg[i + 2] == '/'))
            {
                // Found a drive-letter path. Consume until next path-terminator.
                sb.Append("/mnt/");
                sb.Append(char.ToLowerInvariant(arg[i]));
                int j = i + 2;
                while (j < arg.Length && arg[j] != ',' && arg[j] != '"' && arg[j] != '\0')
                    j++;
                for (int k = i + 2; k < j; k++)
                    sb.Append(arg[k] == '\\' ? '/' : arg[k]);
                i = j;
            }
            else
            {
                sb.Append(arg[i]);
                i++;
            }
        }
        return sb.ToString();
    }
}
