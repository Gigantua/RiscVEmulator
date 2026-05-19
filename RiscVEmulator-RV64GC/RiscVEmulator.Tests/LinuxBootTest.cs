using System.Diagnostics;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

/// <summary>
/// End-to-end integration tests that power on the MMU (Sv32) RV32 Linux
/// kernel through the real <c>Examples.Linux --mmu</c> harness — exercising
/// the Sv32 page-table walk, the emulator-native SBI firmware and the glibc
/// rootfs together.
///
/// The kernel image (<c>~/.cache/riscvemu/linux/Image-mmu</c>) and a built
/// <c>Examples.Linux</c> are produced outside the test harness (buildroot via
/// WSL). When either is absent the tests report <see cref="Assert.Inconclusive"/>
/// rather than failing, so a plain checkout still goes green. The networking
/// test additionally needs outbound internet via the slirp NAT.
/// </summary>
[TestClass]
public class LinuxBootTest
{
    // A glibc Linux boot under the emulator takes a few minutes; allow margin.
    [TestMethod]
    [Timeout(480_000)]
    public void MmuLinux_BootsToShell_AndForks()
    {
        // `sh -c` forks a child shell that exec's and prints the marker. The
        // "42" is computed by the guest, so the full string can only appear
        // as genuine command *output* — never in the echoed command text.
        string log = RunMmuGuest(
            extraArgs: new[] { "--sh" },
            stdinLines: new[]
            {
                @"          sh -c 'echo FORKEXEC_$((6*7))_DONE'",
                "sleep 2",          // let the marker flush before poweroff halts the UART
                "poweroff -f",
            },
            waitMs: 460_000);

        StringAssert.Contains(log, "Run /bin/sh as init process",
            "Kernel did not reach userspace.\n" + Tail(log));
        StringAssert.Contains(log, "FORKEXEC_42_DONE",
            "fork()+execve() of a child process produced no output — fork is broken.\n" + Tail(log));
    }

    [TestMethod]
    [Timeout(480_000)]
    public void MmuLinux_Network_DhcpPingWget()
    {
        // Bring eth0 up, DHCP a lease off the slirp NAT, then prove ICMP and
        // HTTP egress with ping + wget. Leading spaces on the first line are
        // sacrificial against early console-input loss.
        string log = RunMmuGuest(
            extraArgs: new[] { "--gui", "--sh" },   // --gui registers virtio-net
            stdinLines: new[]
            {
                "          ifconfig eth0 up",
                "udhcpc -i eth0 -n -q -t 5",
                "ping -c 3 example.com",
                "wget -O /tmp/ex.html http://example.com 2>&1",
                "sleep 2",          // let ping/wget output flush before poweroff
                "poweroff -f",
            },
            waitMs: 460_000);

        StringAssert.Contains(log, "obtained",
            "DHCP did not obtain a lease.\n" + Tail(log));
        StringAssert.Contains(log, "0% packet loss",
            "ping (ICMP egress) failed.\n" + Tail(log));
        StringAssert.Contains(log, "saved",
            "wget (HTTP egress) did not save the page.\n" + Tail(log));
    }

    [TestMethod]
    [Timeout(60_000)]
    public void Rv64Alpine_BootsToShell()
    {
        // Boot a real Alpine riscv64 rootfs on the RV64GC core and prove the
        // distribution's own package manager runs. "apk-tools" can only appear
        // as genuine output of `apk --version` inside the booted guest.
        // The RV64 boot reaches /init in a few seconds — 45 s is ample.
        string log = RunRv64Guest("apk --version; poweroff -f", waitMs: 45_000);

        StringAssert.Contains(log, "Alpine",
            "RV64 kernel did not reach the Alpine initramfs.\n" + Tail(log));
        StringAssert.Contains(log, "apk-tools",
            "Alpine's package manager (apk) did not run.\n" + Tail(log));
    }

    /// <summary>Launches <c>Examples.Linux --rv64 --auto-commands "&lt;cmds&gt;"</c>,
    /// which boots Alpine riscv64 and injects the commands at the shell prompt.
    /// Reports Inconclusive if the RV64 kernel image or the built example are
    /// missing.</summary>
    private static string RunRv64Guest(string autoCommands, int waitMs)
    {
        string kernel = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".cache", "riscvemu", "linux", "Image-rv64");
        if (!File.Exists(kernel))
            Assert.Inconclusive(
                $"RV64 kernel image not present ({kernel}). " +
                "Build it via buildroot qemu_riscv64_virt_defconfig.");

        string? dll = FindExamplesLinuxDll();
        if (dll == null)
            Assert.Inconclusive("Examples.Linux.dll not found — build the Examples.Linux project.");

        var psi = new ProcessStartInfo("dotnet")
        {
            RedirectStandardInput  = true,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            UseShellExecute        = false,
            WorkingDirectory       = Path.GetDirectoryName(dll)!,
        };
        psi.ArgumentList.Add(dll);
        psi.ArgumentList.Add("--rv64");
        psi.ArgumentList.Add("--auto-commands");
        psi.ArgumentList.Add(autoCommands);

        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to launch Examples.Linux.");

        var output = new StringBuilder();
        void Sink(object _, DataReceivedEventArgs e)
        {
            if (e.Data != null) lock (output) output.Append(e.Data).Append('\n');
        }
        proc.OutputDataReceived += Sink;
        proc.ErrorDataReceived  += Sink;
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        if (!proc.WaitForExit(waitMs))
        {
            try { proc.Kill(entireProcessTree: true); } catch { /* already gone */ }
        }
        proc.WaitForExit();

        lock (output) return output.ToString();
    }

    /// <summary>Launches <c>Examples.Linux --mmu [extraArgs]</c>, feeds
    /// <paramref name="stdinLines"/> to the guest console, waits, and returns
    /// the combined stdout/stderr. Reports Inconclusive if the kernel image or
    /// the built example are missing.</summary>
    private static string RunMmuGuest(string[] extraArgs, string[] stdinLines, int waitMs)
    {
        string kernel = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".cache", "riscvemu", "linux", "Image-mmu");
        if (!File.Exists(kernel))
            Assert.Inconclusive(
                $"MMU kernel image not present ({kernel}). " +
                "Build it via Examples.Linux.Build_RV32i (MMU config).");

        string? dll = FindExamplesLinuxDll();
        if (dll == null)
            Assert.Inconclusive("Examples.Linux.dll not found — build the Examples.Linux project.");

        var psi = new ProcessStartInfo("dotnet")
        {
            RedirectStandardInput  = true,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            UseShellExecute        = false,
            WorkingDirectory       = Path.GetDirectoryName(dll)!,
        };
        psi.ArgumentList.Add(dll);
        psi.ArgumentList.Add("--mmu");
        foreach (string a in extraArgs) psi.ArgumentList.Add(a);

        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to launch Examples.Linux.");

        var output = new StringBuilder();
        void Sink(object _, DataReceivedEventArgs e)
        {
            if (e.Data != null) lock (output) output.Append(e.Data).Append('\n');
        }
        proc.OutputDataReceived += Sink;
        proc.ErrorDataReceived  += Sink;
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        foreach (string line in stdinLines) proc.StandardInput.WriteLine(line);
        proc.StandardInput.Flush();

        if (!proc.WaitForExit(waitMs))
        {
            try { proc.Kill(entireProcessTree: true); } catch { /* already gone */ }
        }
        // Parameterless WaitForExit drains the async stdout/stderr handlers —
        // WaitForExit(int) can return before the last lines reach the sink.
        proc.WaitForExit();

        lock (output) return output.ToString();
    }

    /// <summary>Walks up from the test output directory to the repo root, then
    /// picks an <c>Examples.Linux.dll</c> under <c>Examples/Linux/bin</c>.
    /// A Release build is preferred — a Debug emulator boots Linux several
    /// times slower — and the newest within that preference wins.</summary>
    private static string? FindExamplesLinuxDll()
    {
        string? dir = AppContext.BaseDirectory;
        while (dir != null)
        {
            string binDir = Path.Combine(dir, "Examples", "Linux", "bin");
            if (Directory.Exists(binDir))
            {
                var all = Directory.EnumerateFiles(binDir, "Examples.Linux.dll", SearchOption.AllDirectories);
                return all
                    .OrderByDescending(p => p.Contains($"{Path.DirectorySeparatorChar}Release{Path.DirectorySeparatorChar}"))
                    .ThenByDescending(File.GetLastWriteTimeUtc)
                    .FirstOrDefault();
            }
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    private static string Tail(string s, int lines = 30)
    {
        string[] all = s.Split('\n');
        return string.Join("\n", all.Skip(Math.Max(0, all.Length - lines)));
    }
}
