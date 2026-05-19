using System.IO.Compression;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Networking;
using RiscVEmulator.Core.Peripherals;

// Make stdout auto-flush so prompts without \n (e.g. "login: ", "# ") appear immediately.
Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), Console.OutputEncoding) { AutoFlush = true });
Console.OutputEncoding = System.Text.Encoding.UTF8;

// ── Argument parsing ─────────────────────────────────────────────────────────

string? kernelPath = null;
string? dtbPath    = null;
int     ramMB      = 96;        // 64 MB Linux + 32 MB headroom for Microwindows etc.
bool    doDownload = false;
bool    enableNet  = false;
bool    enableGui  = false;
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
        case "--net":      enableNet  = true; break;
        case "--gui":      enableGui  = true; break;
        default:
            Console.Error.WriteLine($"Unknown option: {args[i]}");
            PrintUsage();
            return 1;
    }
}

// ── Resolve kernel + DTB ─────────────────────────────────────────────────────
//
// Priority order:
//   1. Explicit --kernel / --dtb args (user override).
//   2. Examples.Linux.Build_RV32i outputs (Image-net + rvemu-net.dtb) if they exist —
//      auto-enables --net.
//   3. Downloaded mini-rv32ima image (Image + sixtyfourmb.dtb).
//   4. --download to fetch (3).

string cacheDir = Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cache", "riscvemu", "linux");
Directory.CreateDirectory(cacheDir);

string preparedKernel = Path.Combine(cacheDir, "Image-net");
string preparedDtb    = Path.Combine(cacheDir, "rvemu-net.dtb");
string defaultKernel  = Path.Combine(cacheDir, "Image");
string defaultDtb     = Path.Combine(cacheDir, "sixtyfourmb.dtb");

bool userOverrodeKernel = kernelPath != null;
bool userOverrodeDtb    = dtbPath    != null;

if (!userOverrodeKernel && !userOverrodeDtb &&
    File.Exists(preparedKernel) && File.Exists(preparedDtb))
{
    Console.Error.WriteLine($"Using Build_RV32i kernel: {preparedKernel}");
    Console.Error.WriteLine($"Using Build_RV32i DTB:    {preparedDtb}");
    kernelPath = preparedKernel;
    dtbPath    = preparedDtb;
    enableNet  = true;          // prepared image always has virtio-net.
    enableGui  = true;          // ...and the rvemu-desktop overlay app.
}

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
    Console.Error.WriteLine("Options:");
    Console.Error.WriteLine("  • Run with --download to fetch the pre-built mini-rv32ima Linux image (no networking).");
    Console.Error.WriteLine("  • Run 'dotnet run --project Examples\\Linux.Build_RV32i' to build a networked image via WSL+buildroot.");
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

// Framebuffer slot — last 4 MB of RAM, fixed at 0x85C00000. The FB lives
// INSIDE the RAM bank (vs. a separate physical address like 0x20000000)
// to avoid the init_unavailable_range trap — see CLAUDE.md "Don't put
// simple-framebuffer in the DT" for the post-mortem. /memory is shrunk
// to exclude this region; guest userspace mmap's /dev/mem at FbBase to
// draw. 1024×768×32 = 3 MB, rounded up to 4 MB for headroom.
// Requires ramMB >= 96 so the FB region is actually committed.
const uint FbBase    = 0x85C0_0000u;
const uint FbSize    = 0x400000u;         // 4 MB (1024*768*4 = 3,145,728)
const int  FbWidth   = 1024;
const int  FbHeight  = 768;

if (kernelImage.Length > ramSize)
{
    Console.Error.WriteLine($"Kernel ({kernelImage.Length:N0} bytes) does not fit in {ramMB} MB RAM.");
    return 1;
}
if (RamBase + ramSize < FbBase + FbSize)
{
    Console.Error.WriteLine($"--ram {ramMB} too small; framebuffer at 0x{FbBase:X8} needs RAM ≥ 96 MB.");
    return 1;
}

// DTB: use provided file
byte[] dtbBytes = File.ReadAllBytes(dtbPath);

// ── Memory layout ────────────────────────────────────────────────────────────
//
//   Physical 0x80000000 .. 0x80000000+ramSize
//   [0x00000000          ] kernel flat binary
//   [dtbRamOffset        ] DTB
//   [0x85C00000-0x85FFFFFF] framebuffer  (last 4 MB, fixed; 1024x768x32)
//
// DTB sits BELOW the framebuffer so kernel can read it without colliding.

uint dtbRamOffset = (FbBase - RamBase) - (uint)dtbBytes.Length - 64;

// Native mtime is just the CPU's instruction counter. DTS default
// `timebase-frequency = 1 MHz` makes the kernel think one tick = one μs,
// but at our typical ~60 MIPS that's 60× too fast. Override with a
// rough match. If your host runs faster, bump this — visible symptom is
// nxclock or `date` racing ahead of wall clock. A pre-boot benchmark
// would compute this exactly per host but adds startup cost.
const uint TimebaseHz = 60_000_000u;

// Patch the RAM size field in the DTB. Both rvemu-net.dts and the legacy
// sixtyfourmb.dts use 0x00C0FF03 as a magic guard in <reg = <0x80000000 0x00C0FF03>>;
// at runtime we scan for that big-endian u32 and replace it with the actual
// RAM size in BE so the kernel sees the right amount of memory.
//
// validRam = ramSize − FbSize: SHRINK /memory so the kernel never tracks
// the FB region at all (no struct pages, no slab in those pages).
// Otherwise userspace /dev/mem mmap to the FB clobbers kernel data that
// happened to land in those pages — symptoms: WARN at mm/internal.h,
// slab_common.c, workqueue.c followed by devtmpfs corruption.
// Since FB is OUTSIDE /memory, there's still only ONE memblock bank — the
// gap below `RamBase` is iterated by init_unavailable_range, but with
// ARCH_PFN_OFFSET = 0x80000 every PFN in the hole returns false from
// pfn_valid, so the walk skips by pageblock and finishes fast.
byte[] dtbPatch = (byte[])dtbBytes.Clone();
{
    uint validRam = (uint)ramSize - FbSize;
    for (int o = 0; o + 4 <= dtbPatch.Length; o++)
    {
        uint w = (uint)(dtbPatch[o] << 24 | dtbPatch[o+1] << 16 |
                        dtbPatch[o+2] << 8 | dtbPatch[o+3]);
        if (w != 0x00C0FF03u) continue;
        dtbPatch[o+0] = (byte)(validRam >> 24);
        dtbPatch[o+1] = (byte)(validRam >> 16);
        dtbPatch[o+2] = (byte)(validRam >>  8);
        dtbPatch[o+3] = (byte)(validRam >>  0);
        break;
    }
    // Patch timebase-frequency 0x000F4240 (1 MHz, the cnlohr DTS default)
    // with the measured MIPS so the kernel's clock runs at ~wall pace.
    for (int o = 0; o + 4 <= dtbPatch.Length; o++)
    {
        uint w = (uint)(dtbPatch[o] << 24 | dtbPatch[o+1] << 16 |
                        dtbPatch[o+2] << 8 | dtbPatch[o+3]);
        if (w != 0x000F4240u) continue;
        dtbPatch[o+0] = (byte)((TimebaseHz >> 24) & 0xFF);
        dtbPatch[o+1] = (byte)((TimebaseHz >> 16) & 0xFF);
        dtbPatch[o+2] = (byte)((TimebaseHz >>  8) & 0xFF);
        dtbPatch[o+3] = (byte)((TimebaseHz >>  0) & 0xFF);
        break;
    }
}

// ── Build SoC ────────────────────────────────────────────────────────────────

var memory = new Memory(ramSize, RamBase);
var bus    = new MemoryBus(memory);
var uart   = new UartDevice();
// cnlohr's mini-rv32ima machine layout (used by both the downloaded image and
// the Build_RV32i network-capable kernel): CLINT@0x11000000, SYSCON@0x11100000.
var syscon = new SysconDevice(0x1110_0000u);
var clint  = new ClintDevice (0x1100_0000u);

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(syscon);
bus.RegisterPeripheral(clint);

// Networking (opt-in via --net). Requires a kernel built with CONFIG_VIRTIO_NET.
// Both PLIC and VirtioNet must be registered BEFORE the Emulator is constructed,
// because Emulator's ctor commits each peripheral's MMIO slice via VirtualAlloc
// + MmioDispatcher.Register.
PlicDevice?       plic    = null;
VirtioNetDevice?  virtNet = null;
INetBackend?      backend = null;
if (enableNet)
{
    plic    = new PlicDevice(0x0C00_0000);
    // Prefer libslirp (full TCP/IP+NAT, matches QEMU's -net user) if slirp.dll is
    // present next to the executable; otherwise fall back to our Win32 ARP/ICMP/UDP
    // backend. Drop libslirp-0.dll from MSYS2's mingw-w64-x86_64-libslirp package
    // into the runtime directory to enable the full stack.
    if (SlirpBridgeBackend.IsAvailable())
    {
        Console.Error.WriteLine("Network backend: libslirp via slirp_bridge (TCP/UDP/ICMP/ARP/DHCP/DNS)");
        backend = new SlirpBridgeBackend();
    }
    else
    {
        Console.Error.WriteLine("Network backend: Win32 (ARP/ICMP/UDP — install slirp_bridge.dll for full TCP)");
        backend = new Win32NatBackend();
    }
    virtNet = new VirtioNetDevice(0x1000_8000, plic, irqNum: 1, backend, memory.Reservation.Base);
    bus.RegisterPeripheral(plic);
    bus.RegisterPeripheral(virtNet);
}

// Framebuffer at FbBase (inside the top of RAM). The Memory peripheral
// already committed this region as plain RAM; FramebufferDevice's commit
// is a no-op overlay (VirtualAlloc on already-committed pages just returns
// the same pointer). The result: CPU writes to FbBase land in real RAM,
// SDL sees them via PresentedPixels, AND the kernel's simple-framebuffer
// driver can ioremap the region from /reserved-memory.
var framebuffer = new FramebufferDevice(FbWidth, FbHeight, FbBase);
bus.RegisterPeripheral(framebuffer);

// Keyboard + mouse MMIO peripherals (same as Doom/Voxel use).
// The guest's rvemu-input daemon mmap's /dev/mem at these addresses and
// translates events into /dev/input/event0 + event1 via /dev/uinput so
// any evdev-aware app (Microwindows, fbterm) sees standard Linux input.
var keyboard = new KeyboardDevice();
var mouse    = new MouseDevice();
bus.RegisterPeripheral(keyboard);
bus.RegisterPeripheral(mouse);

// Audio MMIO peripherals (same as Doom uses).
//   0x30000000  AudioBufferDevice  (1 MB plain RAM — PCM samples)
//   0x30100000  AudioControlDevice (guarded — sample rate / play trigger)
// Guest userspace mmap's /dev/mem at these addresses; LinuxSdlAudio drains
// the buffer to SDL2 when the guest sets Ctrl bit 0. Region is outside
// /memory@0x80000000 so it doesn't perturb memblock — no DT entry needed.
var audioBuf  = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();
bus.RegisterPeripheral(audioBuf);
bus.RegisterPeripheral(audioCtrl);

// MIDI MMIO peripheral (same as Examples.Midi).
//   0x10005000  MidiDevice (guarded — write packed short message to +0x04)
// Guest's rvemu-midid daemon reads the rawmidi loopback at /dev/snd/midiC1D0
// (snd-virmidi), parses the byte stream, and writes 3-byte short messages
// here. MidiDevice forwards each to winmm.midiOutShortMsg() → Windows GM
// synth. On non-Windows hosts midiOutGetNumDevs() returns 0 and all writes
// silently no-op, so registering this is safe everywhere.
var midi = new MidiDevice();
bus.RegisterPeripheral(midi);

// Load kernel at physical 0x80000000
bus.Load(RamBase, kernelImage, 0, kernelImage.Length);

// Load patched DTB near end of RAM
bus.Load(RamBase + dtbRamOffset, dtbPatch, 0, dtbPatch.Length);

// ── Create emulator ──────────────────────────────────────────────────────────

var regs = new RegisterFile();
regs.Write(10, 0);                          // a0 = hart ID = 0
regs.Write(11, RamBase + dtbRamOffset);     // a1 = physical DTB address

var emu = new Emulator(bus, regs, RamBase);
emu.RamOffset        = RamBase;
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

// Forward Ctrl-C to the guest (byte 0x03 on stdin → UART → busybox tty
// discipline → SIGINT to foreground process). Without this the .NET
// runtime catches Ctrl-C as a process-level signal and the guest shell
// never sees it. To exit the emulator: `poweroff` from inside the guest,
// or close the host terminal window.
try { Console.TreatControlCAsInput = true; } catch { /* not a console */ }

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

// Optional SDL viewer — renders FB + forwards keyboard/mouse events to the
// guest's MMIO peripherals. The rvemu-input daemon inside the guest then
// translates those to /dev/input/eventN. Audio drain runs on its own thread
// so guests can play sound without --gui implying a window in the future.
Examples.Linux.LinuxSdlViewer? viewer = null;
Examples.Linux.LinuxSdlAudio?  audio  = null;
if (enableGui)
{
    viewer = new Examples.Linux.LinuxSdlViewer(framebuffer, keyboard, mouse,
        title: $"rvemu Linux ({ramMB} MB)", scale: 1);
    viewer.Start();
    audio  = new Examples.Linux.LinuxSdlAudio(audioBuf, audioCtrl);
    audio.Start();
    Console.Error.WriteLine("SDL viewer + audio started (--gui).");
}

const int BatchSize = 500_000;

// Diagnostic heartbeat: when RVEMU_TRACE=1, print mtime+PC every 2s on stderr.
// Useful when boot appears to hang — shows whether the CPU is stuck in a
// tight loop (specific PC range) or making forward progress. Was instrumental
// in pinning down the simple-framebuffer/init_unavailable_range bug.
bool trace = Environment.GetEnvironmentVariable("RVEMU_TRACE") == "1";
if (trace)
{
    new System.Threading.Thread(() =>
    {
        while (!cts.IsCancellationRequested && !emu.IsHalted)
        {
            System.Threading.Thread.Sleep(2000);
            if (cts.IsCancellationRequested || emu.IsHalted) break;
            Console.Error.WriteLine($"[trace] mtime={emu.MTime:N0} pc=0x{emu.PC:X8}");
        }
    }) { IsBackground = true, Name = "Heartbeat" }.Start();
}

while (!emu.IsHalted && !cts.IsCancellationRequested)
{
    emu.StepN(BatchSize);
}

cts.Cancel();
viewer?.Stop();
audio?.Stop();
midi.Dispose();
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
    Console.Error.WriteLine("  --net             Enable virtio-net (requires kernel rebuilt with CONFIG_VIRTIO_NET");
    Console.Error.WriteLine("                    and a DTB compiled from Examples/Linux/rvemu-net.dts).");
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

// ── Windows VT / ANSI console support ────────────────────────────────────────

