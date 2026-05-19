// Examples.Linux.Build_RV64 — recreates the RV64 Linux boot image used by
// `Examples.Linux --rv64`. Drives WSL to produce three artifacts into
// ~/.cache/riscvemu/linux/ :
//
//   1. Image-rv64           — the kernel (buildroot 2024.05.3, linux 6.6.18)
//   2. rvemu-rv64.dtb       — device tree (dtc from Examples/Linux/rvemu-rv64.dts)
//   3. initramfs-rv64.cpio  — Alpine 3.23.4 riscv64 minirootfs + /init +
//                             install-xfce.sh + the rvemu-rv64-{input,audio}
//                             guest daemons (cross-compiled static)
//
// The fourth piece — the persistent ext4 disk rv64-disk.img — is NOT built
// here: it is created on the first `Examples.Linux --rv64` run, when the
// initramfs hands control to install-xfce.sh and apk installs the full
// Alpine + IceWM desktop. Delete rv64-disk.img to force a fresh install.
//
// Re-runnable and idempotent: each step is skipped if its output already
// exists, unless --force (or the step-specific flag) is given.
//
// Usage:
//   dotnet run --project Examples\Linux.Build_RV64                 # build all missing artifacts
//   dotnet run --project Examples\Linux.Build_RV64 -- --force      # rebuild everything
//   dotnet run --project Examples\Linux.Build_RV64 -- --kernel     # kernel only
//   dotnet run --project Examples\Linux.Build_RV64 -- --initramfs  # initramfs only
//   dotnet run --project Examples\Linux.Build_RV64 -- --dtb        # dtb only
//   dotnet run --project Examples\Linux.Build_RV64 -- -j 16        # buildroot parallelism

using System.Diagnostics;

const string BuildrootBranch = "2024.05.x";
const string BuildrootRepo   = "https://gitlab.com/buildroot.org/buildroot.git";
const string WslDistro       = "Ubuntu-24.04";
const string AlpineVersion   = "3.23.4";
const string AlpineMirror    = "http://dl-cdn.alpinelinux.org/alpine";

// ── args ─────────────────────────────────────────────────────────────────
bool force = false, only = false, doKernel = false, doDtb = false, doInitramfs = false;
int  jobs  = Math.Max(2, Environment.ProcessorCount);
for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--force":     force       = true; break;
        case "--kernel":    doKernel    = true; only = true; break;
        case "--dtb":       doDtb       = true; only = true; break;
        case "--initramfs": doInitramfs = true; only = true; break;
        case "-j": case "--jobs": jobs  = int.Parse(args[++i]); break;
        case "-h": case "--help":
            Console.WriteLine("Usage: Build_RV64 [--force] [--kernel|--dtb|--initramfs] [-j N]");
            return 0;
        default:
            Console.Error.WriteLine($"Unknown option: {args[i]}");
            return 1;
    }
}
if (!only) { doKernel = doDtb = doInitramfs = true; }

// ── paths ────────────────────────────────────────────────────────────────
string exeDir = AppContext.BaseDirectory;
string? root  = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
    root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("Cannot find RiscVEmulator.sln."); return 1; }

string projDir  = Path.Combine(root, "Examples", "Linux.Build_RV64");
string linuxDir = Path.Combine(root, "Examples", "Linux");

// WSL-side (/mnt/c/...) views of the repo and the host cache.
string wRoot   = WinToWsl(root);
string wProj   = $"{wRoot}/Examples/Linux.Build_RV64";
string wLinux  = $"{wRoot}/Examples/Linux";
string wCache  = WinToWsl(Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cache", "riscvemu", "linux"));
const string Buildroot = "$HOME/rvemu-buildroot";
const string Output    = "$HOME/rvemu-buildroot/output-rv64";

if (!HasWsl()) { Console.Error.WriteLine($"WSL distro {WslDistro} not found."); return 1; }
WslSh($"mkdir -p {wCache}");
Console.WriteLine($"Build_RV64 — repo {wRoot}\n  cache  {wCache}\n  jobs   {jobs}\n");

// ── 1. kernel ────────────────────────────────────────────────────────────
if (doKernel)
{
    if (!force && WslExists($"{wCache}/Image-rv64"))
        Console.WriteLine("[kernel] Image-rv64 present — skip (use --force to rebuild).");
    else
    {
        Console.WriteLine("[kernel] building via buildroot — this takes 30-90 min...");
        WslSh($$"""
            set -e
            if [ ! -d {{Buildroot}}/.git ]; then
                echo '[kernel] cloning buildroot {{BuildrootBranch}}...'
                git clone --depth 1 -b {{BuildrootBranch}} {{BuildrootRepo}} {{Buildroot}}
            fi
            mkdir -p {{Output}}
            cp {{wProj}}/buildroot-rv64.config {{Output}}/.config
            # pin the kernel .config to the repo copy so the build is self-contained
            sed -i 's#^BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=.*#BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="{{wProj}}/linux-rv64.config"#' {{Output}}/.config
            make -C {{Buildroot}} O={{Output}} olddefconfig
            make -C {{Buildroot}} O={{Output}} -j{{jobs}}
            cp {{Output}}/images/Image {{wCache}}/Image-rv64
            echo "[kernel] done: $(stat -c %s {{wCache}}/Image-rv64) bytes"
            """);
    }
}

// ── 2. device tree ───────────────────────────────────────────────────────
if (doDtb)
{
    if (!force && WslExists($"{wCache}/rvemu-rv64.dtb"))
        Console.WriteLine("[dtb] rvemu-rv64.dtb present — skip.");
    else
    {
        Console.WriteLine("[dtb] compiling rvemu-rv64.dts...");
        WslSh($"dtc -I dts -O dtb -o {wCache}/rvemu-rv64.dtb {wLinux}/rvemu-rv64.dts");
    }
}

// ── 3. initramfs ─────────────────────────────────────────────────────────
if (doInitramfs)
{
    if (!force && WslExists($"{wCache}/initramfs-rv64.cpio"))
        Console.WriteLine("[initramfs] initramfs-rv64.cpio present — skip.");
    else
    {
        Console.WriteLine("[initramfs] cross-compiling guest daemons + assembling cpio...");
        string gcc = $"{Output}/host/bin/riscv64-buildroot-linux-gnu-gcc";
        WslSh($$"""
            set -e
            GCC={{gcc}}
            if [ ! -x "$(echo {{gcc}} | sed "s#\$HOME#$HOME#")" ]; then
                echo 'ERROR: riscv64 toolchain missing — run the kernel build first.'; exit 1
            fi
            stage=$(mktemp -d)
            cd "$stage"
            # Alpine minirootfs
            tb=alpine-minirootfs-{{AlpineVersion}}-riscv64.tar.gz
            wget -q {{AlpineMirror}}/v{{AlpineVersion.Substring(0, AlpineVersion.LastIndexOf('.'))}}/releases/riscv64/$tb
            tar xzf "$tb" && rm "$tb"
            # custom init + installer
            cp {{wProj}}/initramfs-init.sh ./init && chmod +x ./init
            cp {{wLinux}}/install-xfce.sh ./install-xfce.sh && chmod +x ./install-xfce.sh
            sed -i 's/\r$//' ./init ./install-xfce.sh
            # guest daemons — static so they run on musl Alpine
            $GCC -static -O2 {{wLinux}}/rvemu-rv64-input.c -o ./rvemu-rv64-input
            $GCC -static -O2 {{wLinux}}/rvemu-rv64-audio.c -o ./rvemu-rv64-audio
            find . | cpio -o -H newc 2>/dev/null > {{wCache}}/initramfs-rv64.cpio
            cd / && rm -rf "$stage"
            echo "[initramfs] done: $(stat -c %s {{wCache}}/initramfs-rv64.cpio) bytes"
            """);
    }
}

Console.WriteLine("\nBuild_RV64 complete. Boot with:  Examples.Linux --rv64 --gui");
Console.WriteLine("(delete ~/.cache/riscvemu/linux/rv64-disk.img to force a fresh disk install.)");
return 0;

// ── helpers ──────────────────────────────────────────────────────────────
static string WinToWsl(string p) =>
    "/mnt/" + char.ToLower(p[0]) + p.Substring(2).Replace('\\', '/');

static bool HasWsl()
{
    try { return RunCapture("wsl", "-d", WslDistro, "--", "true") == 0; }
    catch { return false; }
}

static bool WslExists(string path) =>
    RunCapture("wsl", "-d", WslDistro, "--", "test", "-e", path) == 0;

// Run a bash script in WSL by writing it to a temp file (avoids every layer
// of Windows/WSL/bash quoting). Streams output; throws on non-zero exit.
static void WslSh(string script)
{
    string tmp = Path.GetTempFileName();
    // Reset PATH to Linux-only — WSL inherits the Windows PATH, whose
    // "Program Files" entries contain spaces, which buildroot rejects.
    const string cleanPath = "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n";
    File.WriteAllText(tmp, "#!/bin/sh\n" + cleanPath + script.Replace("\r\n", "\n"));
    try
    {
        string wtmp = WinToWsl(tmp);
        int rc = Run("wsl", "-d", WslDistro, "--", "bash", wtmp);
        if (rc != 0) throw new Exception($"WSL step failed (exit {rc}).");
    }
    finally { File.Delete(tmp); }
}

static int Run(string file, params string[] a)
{
    var psi = new ProcessStartInfo(file) { UseShellExecute = false };
    foreach (var x in a) psi.ArgumentList.Add(x);
    using var p = Process.Start(psi)!;
    p.WaitForExit();
    return p.ExitCode;
}

static int RunCapture(string file, params string[] a)
{
    var psi = new ProcessStartInfo(file)
    { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
    foreach (var x in a) psi.ArgumentList.Add(x);
    using var p = Process.Start(psi)!;
    p.StandardOutput.ReadToEnd();
    p.StandardError.ReadToEnd();
    p.WaitForExit();
    return p.ExitCode;
}
