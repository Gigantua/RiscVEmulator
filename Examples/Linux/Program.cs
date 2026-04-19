using System.IO.Compression;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

// Make stdout auto-flush so prompts without \n (e.g. "login: ", "# ") appear immediately.
Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), Console.OutputEncoding) { AutoFlush = true });
Console.OutputEncoding = System.Text.Encoding.UTF8;

// ── Argument parsing ─────────────────────────────────────────────────────────

string? kernelPath = null;
string? dtbPath    = null;
int     ramMB      = 128;
bool    doDownload = false;
using var http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = true })
{
    Timeout = TimeSpan.FromMinutes(10),
    DefaultRequestHeaders = { { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36" } }
};

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--kernel":   kernelPath = args[++i]; break;
        case "--dtb":      dtbPath    = args[++i]; break;
        case "--ram":      ramMB      = int.Parse(args[++i]); break;
        case "--download": doDownload = true; break;
        default:
            Console.Error.WriteLine($"Unknown option: {args[i]}");
            PrintUsage();
            return 1;
    }
}

// ── Download pre-built images if requested ───────────────────────────────────

string cacheDir = Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cache", "riscvemu", "linux");
Directory.CreateDirectory(cacheDir);

string defaultKernel = Path.Combine(cacheDir, "Image");
string defaultDtb    = Path.Combine(cacheDir, "sixtyfourmb.dtb");

if (doDownload)
{
    await DownloadKernel(defaultKernel);
    await DownloadDtb(defaultDtb);
}

kernelPath ??= defaultKernel;
dtbPath    ??= defaultDtb;

if (!File.Exists(kernelPath))
{
    Console.Error.WriteLine($"Kernel image not found: {kernelPath}");
    Console.Error.WriteLine("Run with --download to fetch the pre-built mini-rv32ima Linux image.");
    return 1;
}

if (!File.Exists(dtbPath))
{
    Console.Error.WriteLine($"DTB not found: {dtbPath}");
    Console.Error.WriteLine("Run with --download to fetch the pre-built DTB.");
    return 1;
}

// ── Load kernel and DTB images ───────────────────────────────────────────────

byte[] kernelImage = File.ReadAllBytes(kernelPath);
int    ramSize     = ramMB * 1024 * 1024;
const uint RamBase = 0x80000000u;

if (kernelImage.Length > ramSize)
{
    Console.Error.WriteLine($"Kernel ({kernelImage.Length:N0} bytes) does not fit in {ramMB} MB RAM.");
    return 1;
}

// DTB: use provided file
byte[] dtbBytes = File.ReadAllBytes(dtbPath);

// ── Memory layout ────────────────────────────────────────────────────────────
//
//   Physical 0x80000000 .. 0x80000000+ramSize
//   Bus address = physical - 0x80000000
//   [bus 0x00000000] kernel flat binary
//   [bus ram_size - dtb_size - 64] DTB  (near end of RAM, like mini-rv32ima)

uint dtbRamOffset = (uint)(ramSize - dtbBytes.Length - 64);

// Patch the RAM size field in the DTB (big-endian u32 at DTB word offset 0x13C).
// The magic guard value 0x00C0FF03 is placed by sixtyfourmb.dts; replace it
// with the actual usable RAM size in big-endian so the kernel reports it correctly.
byte[] dtbPatch = (byte[])dtbBytes.Clone();
const int PatchOff = 0x13C;
if (PatchOff + 4 <= dtbPatch.Length)
{
    uint guard = (uint)(dtbPatch[PatchOff] << 24 | dtbPatch[PatchOff+1] << 16 |
                         dtbPatch[PatchOff+2] << 8 | dtbPatch[PatchOff+3]);
    if (guard == 0x00C0FF03u)
    {
        uint validRam = dtbRamOffset;  // how much RAM the kernel should see
        dtbPatch[PatchOff+0] = (byte)(validRam >> 24);
        dtbPatch[PatchOff+1] = (byte)(validRam >> 16);
        dtbPatch[PatchOff+2] = (byte)(validRam >>  8);
        dtbPatch[PatchOff+3] = (byte)(validRam >>  0);
    }
}

// ── Build SoC ────────────────────────────────────────────────────────────────

var memory = new Memory(ramSize);
var bus    = new MemoryBus(memory);
var uart   = new UartDevice();
var syscon = new SysconDevice();      // poweroff/reboot at 0x11100000

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(syscon);

// Load kernel at bus address 0 (= physical 0x80000000)
bus.Load(0, kernelImage, 0, kernelImage.Length);

// Load patched DTB near end of RAM
bus.Load(dtbRamOffset, dtbPatch, 0, dtbPatch.Length);

// ── Create emulator ──────────────────────────────────────────────────────────

var regs = new RegisterFile();
regs.Write(10, 0);                          // a0 = hart ID = 0
regs.Write(11, RamBase + dtbRamOffset);     // a1 = physical DTB address

var emu = new Emulator(bus, regs, RamBase);
emu.RamOffset        = RamBase;
emu.EnableMExtension = true;
emu.EnableAExtension = true;
emu.EnablePrivMode   = true;

// Enable ANSI VT sequences on Windows (shell prompt, colours, cursor movement)
ConsoleHelper.EnableVt();

// UART output → console. AutoFlush is set at startup, so every char appears immediately.
uart.OutputHandler = c => Console.Write(c);

// Halt when Linux powers off
syscon.OnPowerOff = () => emu.SetHalted(true);
syscon.OnReboot   = () => emu.SetHalted(true);

// ── Keyboard input thread ────────────────────────────────────────────────────

var cts = new System.Threading.CancellationTokenSource();

Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    Console.Error.WriteLine("\nCtrl+C — halting emulator.");
    cts.Cancel();
};

var stdinThread = new System.Threading.Thread(() =>
{
    try
    {
        var stdin = Console.OpenStandardInput();
        var buf   = new byte[64];
        int n;
        while (!cts.IsCancellationRequested && (n = stdin.Read(buf, 0, buf.Length)) > 0)
        {
            for (int j = 0; j < n; j++)
                uart.EnqueueInput(buf[j] == 13 ? (byte)10 : buf[j]); // CR→LF
        }
    }
    catch { /* stdin closed */ }
}) { IsBackground = true, Name = "StdinReader" };
stdinThread.Start();

// ── Run loop ─────────────────────────────────────────────────────────────────

Console.Error.WriteLine($"Booting Linux kernel ({kernelImage.Length:N0} bytes) with {ramMB} MB RAM at 0x{RamBase:X8}...");
Console.Error.WriteLine("Press Ctrl+C to exit emulator (the signal is NOT forwarded to Linux).");
Console.Error.WriteLine();

const int BatchSize = 500_000;

while (!emu.IsHalted && !cts.IsCancellationRequested)
{
    emu.StepN(BatchSize);
}

cts.Cancel();
Console.Error.WriteLine($"\nEmulator stopped. Executed ~{emu.MTime:N0} instructions.");
return 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

static void PrintUsage()
{
    Console.Error.WriteLine("Usage: Examples.Linux [options]");
    Console.Error.WriteLine("  --kernel <path>   Flat binary kernel image (default: ~/.cache/riscvemu/linux/Image)");
    Console.Error.WriteLine("  --dtb    <path>   Device tree blob (default: ~/.cache/riscvemu/linux/sixtyfourmb.dtb)");
    Console.Error.WriteLine("  --ram    <MB>     RAM size in MB (default: 128)");
    Console.Error.WriteLine("  --download        Download pre-built mini-rv32ima Linux image (~6 MB)");
}

async Task DownloadKernel(string targetPath)
{
    if (File.Exists(targetPath))
    {
        Console.Error.WriteLine($"Kernel already cached at {targetPath}");
        return;
    }
    Console.Error.Write("Downloading Linux kernel image... ");
    // Use raw.githubusercontent.com directly to avoid github.com redirect
    const string ZipUrl = "https://raw.githubusercontent.com/cnlohr/mini-rv32ima-images/master/images/linux-6.1.14-rv32nommu-cnl-1.zip";
    string tmpZip = targetPath + ".zip.tmp";
    using (var resp = await http.GetAsync(ZipUrl, HttpCompletionOption.ResponseHeadersRead))
    {
        resp.EnsureSuccessStatusCode();
        using var fs = File.Create(tmpZip);
        await resp.Content.CopyToAsync(fs);
    }
    using var archive = ZipFile.OpenRead(tmpZip);
    var entry = archive.GetEntry("Image") ?? archive.Entries[0];
    entry.ExtractToFile(targetPath, overwrite: true);
    File.Delete(tmpZip);
    Console.Error.WriteLine($"done ({new FileInfo(targetPath).Length:N0} bytes -> {targetPath})");
}

async Task DownloadDtb(string targetPath)
{
    if (File.Exists(targetPath))
    {
        Console.Error.WriteLine($"DTB already cached at {targetPath}");
        return;
    }
    Console.Error.Write("Downloading DTB... ");
    const string DtbUrl = "https://raw.githubusercontent.com/cnlohr/mini-rv32ima/master/mini-rv32ima/sixtyfourmb.dtb";
    using var resp = await http.GetAsync(DtbUrl, HttpCompletionOption.ResponseHeadersRead);
    resp.EnsureSuccessStatusCode();
    using var fs = File.Create(targetPath);
    await resp.Content.CopyToAsync(fs);
    Console.Error.WriteLine($"done ({new FileInfo(targetPath).Length:N0} bytes -> {targetPath})");
}

// ── SYSCON peripheral (poweroff/reboot at 0x11100000) ────────────────────────

public sealed class SysconDevice : IPeripheral
{
    public uint BaseAddress => 0x11100000u;
    public uint Size        => 0x1000u;

    public Action? OnPowerOff { get; set; }
    public Action? OnReboot   { get; set; }

    public uint Read(uint offset, int width) => 0;

    public void Write(uint offset, int width, uint value)
    {
        if (offset != 0) return;
        if      (value == 0x5555u) OnPowerOff?.Invoke();
        else if (value == 0x7777u) OnReboot?.Invoke();
    }
}

// ── Windows VT / ANSI console support ────────────────────────────────────────

/// <summary>Enables ANSI/VT-100 escape sequence processing on Windows stdout.</summary>
internal static class ConsoleHelper
{
    internal static void EnableVt()
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
