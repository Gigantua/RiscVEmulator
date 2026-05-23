using System.IO.Compression;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Networking;
using RiscVEmulator.Core.Peripherals;

// JIT default: OFF for Linux. The JIT path has an unresolved SEH-unwind /
// GC-walker race that crashes the host process when guest workloads do
// heavy MMIO (most visibly: launching Doom inside Linux). Interpreter mode
// is stable end-to-end. Other examples (Doom standalone, Quake, TinyCC,
// Voxel) leave JIT enabled — only the Linux example forces JIT=0.
// Override here with RVEMU_JIT=1 if you want to bench/profile and accept
// the crash risk.
if (Environment.GetEnvironmentVariable("RVEMU_JIT") == null)
{
    Environment.SetEnvironmentVariable("RVEMU_JIT", "0");
}

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
bool    disableGui = false;
string? autoCommands = Environment.GetEnvironmentVariable("RVEMU_AUTO_COMMANDS");
string? haltOnOutput = Environment.GetEnvironmentVariable("RVEMU_HALT_ON_OUTPUT");
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
        case "--no-gui":   disableGui = true; break;
        case "--auto-commands": autoCommands = args[++i]; break;
        case "--halt-on-output": haltOnOutput = args[++i]; break;
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

// Kernel + DTB are embedded as resources in this .dll (see .csproj
// EmbeddedResource entries). Materialize them to a per-user temp dir
// on first run so the rest of the example uses regular file paths.
// This avoids the parallel-WSL-session cache-clobber problem AND
// doesn't depend on working dir / launch path / build config.
string repoKernel;
string repoDtb;
{
    string snapDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "rvemu", "linux-snapshot");
    Directory.CreateDirectory(snapDir);
    repoKernel = Path.Combine(snapDir, "Image-net");
    repoDtb    = Path.Combine(snapDir, "rvemu-net.dtb");
    var asm = typeof(Program).Assembly;
    foreach (var (resName, outPath) in new[] {
        ("Examples.Linux.kernel.Image-net",     repoKernel),
        ("Examples.Linux.kernel.rvemu-net.dtb", repoDtb),
    })
    {
        using var s = asm.GetManifestResourceStream(resName);
        if (s == null) continue;
        // Only rewrite if size differs — saves disk + plays nice with
        // file-modification-time checks downstream.
        if (!File.Exists(outPath) || new FileInfo(outPath).Length != s.Length)
        {
            using var fs = File.Create(outPath);
            s.CopyTo(fs);
            Console.Error.WriteLine($"[snapshot] extracted {Path.GetFileName(outPath)} ({s.Length:N0} B) -> {outPath}");
        }
    }
}
string preparedKernel = File.Exists(repoKernel) ? repoKernel : Path.Combine(cacheDir, "Image-net");
string preparedDtb    = File.Exists(repoDtb)    ? repoDtb    : Path.Combine(cacheDir, "rvemu-net.dtb");
string defaultKernel  = Path.Combine(cacheDir, "Image");
string defaultDtb     = Path.Combine(cacheDir, "sixtyfourmb.dtb");

bool userOverrodeKernel = kernelPath != null;
bool userOverrodeDtb    = dtbPath    != null;

if (!userOverrodeKernel && !userOverrodeDtb &&
    File.Exists(preparedKernel) && File.Exists(preparedDtb))
{
    string srcLabel = preparedKernel == repoKernel ? "repo" : "Build_RV32i";
    Console.Error.WriteLine($"Using {srcLabel} kernel: {preparedKernel}");
    Console.Error.WriteLine($"Using {srcLabel} DTB:    {preparedDtb}");
    kernelPath = preparedKernel;
    dtbPath    = preparedDtb;
    enableNet  = true;          // prepared image always has virtio-net.
    enableGui  = true;          // ...and the rvemu-desktop overlay app.
}

if (disableGui)
    enableGui = false;

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

// Migrate any legacy paravirt-IRQ kernel that hard-coded the trap-frame
// base at 0x10006000 to the new TrapFrameDevice base at 0x0F000000.
// Build_RV32i (May-17 era) emitted `lui t0, 0x10006` everywhere the
// paravirt-IRQ device was poked; the new native trap unit only knows
// about 0x0F000000, so without this patch every `csrw CSR_TVEC, …`
// turned into a write into MMIO void, leaving TRAP_VECTOR=0 forever.
// The instruction format is `imm[31:12] | rd[11:7] | opcode[6:0]=0x37`.
// Mask off rd+opcode, compare upper 20 bits to 0x10006, rewrite to
// 0x0F000. addi/sw/lw instructions that follow already encode the
// correct byte offsets (0x004 = TVEC, 0x008 = IE_MASK, 0x00C = SCRATCH).
{
    int rewrites = 0;
    for (int i = 0; i + 3 < kernelImage.Length; i += 4)
    {
        uint w = BitConverter.ToUInt32(kernelImage, i);
        if ((w & 0xFFFFF07F) == 0x10006037)         // lui rd, 0x10006
        {
            uint patched = (w & 0x00000FFF) | (0x0F000u << 12);
            BitConverter.GetBytes(patched).CopyTo(kernelImage, i);
            rewrites++;
        }
    }
    if (rewrites > 0)
        Console.Error.WriteLine($"[trap-frame migrator] rewrote {rewrites} legacy `lui rd, 0x10006` -> `lui rd, 0x0F000`");

}

// Patch out auto-launch of Doom from the embedded init script (S45microwindows).
// The script tests `[ -x /usr/bin/doom ]` before forking doom. Renaming the
// path to /usr/bin/noex (same byte length) makes the test fail and the
// surrounding block is skipped.
//
// Separately: the doom wrapper at /usr/bin/doom execs `/usr/libexec/doom-puredoom`,
// but the actual binary installed is `/usr/libexec/doom` (the doomgeneric/
// doom-puredoom recipe mismatch). Patch the wrapper to point at the correct
// path — same byte length via padding spaces.
static void PatchAll(byte[] image, byte[] needle, byte[] replacement, string label)
{
    if (needle.Length != replacement.Length)
        throw new InvalidOperationException("Patch needle/replacement must be same length");
    int patches = 0;
    for (int i = 0; i + needle.Length <= image.Length; i++)
    {
        bool match = true;
        for (int j = 0; j < needle.Length; j++)
            if (image[i + j] != needle[j]) { match = false; break; }
        if (match)
        {
            replacement.CopyTo(image, i);
            patches++;
            i += needle.Length - 1;
        }
    }
    if (patches > 0)
        Console.Error.WriteLine($"[{label}] patched {patches} site(s)");
}

PatchAll(kernelImage,
    System.Text.Encoding.ASCII.GetBytes("[ -x /usr/bin/doom ]"),
    System.Text.Encoding.ASCII.GetBytes("[ -x /usr/bin/noex ]"),
    "auto-doom-disable");

// `S45microwindows stop` ends with `killall doom nxeyes nxclock ...`. The
// doom-launch wrapper at /usr/bin/doom calls `S45microwindows stop` before
// invoking the engine — and `killall doom` matches the wrapper SHELL process
// because the script is *named* /usr/bin/doom (busybox sets argv[0] from the
// scriptname). Result: wrapper SIGTERMs itself before reaching the engine.
//
// Fix: blank out "doom " in the killall list so the wrapper isn't killed.
// Same byte length (5→5 spaces), no other side effect — the real doom
// process gets killed by the for-loop above via /var/run/doom.pid if it's
// running.
PatchAll(kernelImage,
    System.Text.Encoding.ASCII.GetBytes("killall doom nxeyes"),
    System.Text.Encoding.ASCII.GetBytes("killall      nxeyes"),
    "doom-self-kill-fix");

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
    if (disableGui)
        ReplaceAscii(dtbPatch, "rvemu.nogui=0", "rvemu.nogui=1");

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
var trapFrame = new TrapFrameDevice();  // hardware trap-frame page at 0x0F000000

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(syscon);
bus.RegisterPeripheral(clint);
bus.RegisterPeripheral(trapFrame);

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


// Enable ANSI VT sequences on Windows (shell prompt, colours, cursor movement)
ConsoleHelper.EnableVt();

// UART output → console. AutoFlush is set at startup, so every char appears immediately.
// In automation mode, detect the BusyBox prompt and inject commands immediately
// instead of relying on fixed sleeps or interactive shell polling.
//
// The two match-loops below MUST be allocation-free per byte. The original
// implementation did `new string(queue.ToArray())` per byte — at kernel-
// printk-storm rates (tens of MB/s) that pushed Gen0 every few ms, and a
// GC fired from the drain thread suspended the CPU thread mid-JIT-frame
// long enough for the stack walker to race the VEH-injected exception
// frame → STATUS_STACK_BUFFER_OVERRUN (0xc0000409).
//
// Replacement: a per-pattern integer cursor. Advance on match, rewind to
// the longest proper suffix that is still a prefix (KMP-lite — but our
// patterns have no internal repetition, so the rewind is just "back to
// the longest matching prefix of length k for some k < cur"). All zero
// alloc, all branch-friendly.
string?  haltPattern    = haltOnOutput;        // null when no halt-on-token mode
int      haltCur        = 0;
bool     haltTokenSeen  = false;
string[] promptPatterns = { "~ #", "~#", "# " };
int[]    promptCur      = new int[promptPatterns.Length];
bool     autoCommandsSent = false;

static int RewindMatch(string pat, int cur, char c)
{
    // After a mismatch at position `cur` on input char `c`, find the longest
    // proper suffix of pat[0..cur] that is also a prefix of pat AND extends
    // by `c`. For our short patterns ("~ #", "~#", "# ") we just retry the
    // shorter prefixes linearly — at most pat.Length iterations.
    while (cur > 0)
    {
        cur--;
        if (cur == 0) return (c == pat[0]) ? 1 : 0;
        // Check whether pat[0..cur] is a suffix of the consumed input.
        // Cheap heuristic for our 2-3 char patterns: just try pat[cur]==c.
        if (c == pat[cur]) return cur + 1;
    }
    return (c == pat[0]) ? 1 : 0;
}

// Raw-byte path: avoids per-char Console.Write boxing/locking that would
// otherwise be reached from the VEH callback's UART write. The drain
// thread writes byte[] directly to stdout — zero allocation per char.
uart.OutputStream = Console.OpenStandardOutput();

// The match-loop callbacks (autoCommands / haltOnOutput) still run, but
// now off the VEH path on the drain thread. The OutputHandler hook
// fires per-char from drain — must remain allocation-free per char.
uart.OutputHandler = c =>
{
    if (haltPattern != null && !haltTokenSeen)
    {
        if (c == haltPattern[haltCur])
        {
            haltCur++;
            if (haltCur == haltPattern.Length)
            {
                haltTokenSeen = true;
                emu.SetHalted(true);
            }
        }
        else if (haltCur > 0)
        {
            haltCur = RewindMatch(haltPattern, haltCur, c);
        }
    }

    if (autoCommands == null || autoCommandsSent) return;
    for (int i = 0; i < promptPatterns.Length; i++)
    {
        var p   = promptPatterns[i];
        int cur = promptCur[i];
        if (c == p[cur])
        {
            cur++;
            if (cur == p.Length)
            {
                autoCommandsSent = true;
                ThreadPool.QueueUserWorkItem(_ => EnqueueGuestInput(uart, autoCommands));
                return;
            }
        }
        else if (cur > 0)
        {
            cur = RewindMatch(p, cur, c);
        }
        promptCur[i] = cur;
    }
};

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

// Put the Windows console *input* handle into raw pass-through mode: echo OFF
// (the guest tty echoes — avoids double echo), line-input OFF (char-at-a-time),
// processed-input OFF (Ctrl+C passes through as byte 0x03), virtual-terminal-input
// ON (arrow/function keys arrive as ANSI escape bytes the StdinReader forwards).
// Called AFTER TreatControlCAsInput so the raw SetConsoleMode is not overwritten.
ConsoleHelper.EnableRawInput();
AppDomain.CurrentDomain.ProcessExit += (_, _) => ConsoleHelper.RestoreInput();

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
                uart.EnqueueInput(NormalizeGuestInputByte(buf[j]));
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
        unsafe
        {
            byte* host = (byte*)memory.Reservation.Base;
            uint* tf   = (uint*)(host + 0x0F000000);   // trap-frame page
            uint  tvecPeak = 0;
            uint  tvecAtPeak = 0;
            // Aggressive sub-200ms poll of TRAP_VECTOR so we catch the
            // value the kernel writes before it's cleared.
            new System.Threading.Thread(() => {
                while (!cts.IsCancellationRequested && !emu.IsHalted)
                {
                    uint v = tf[1];
                    if (v != 0 && v != tvecPeak)
                    {
                        tvecPeak = v;
                        tvecAtPeak = (uint)emu.MTime;
                        Console.Error.WriteLine($"[trap-watch] TVEC set to 0x{v:X8} @ mtime~{tvecAtPeak}");
                    }
                    System.Threading.Thread.Sleep(50);
                }
            }) { IsBackground = true, Name = "TrapVecWatch" }.Start();
            while (!cts.IsCancellationRequested && !emu.IsHalted)
            {
                System.Threading.Thread.Sleep(2000);
                if (cts.IsCancellationRequested || emu.IsHalted) break;
                uint ieFlag  = tf[0];                  // +0x000  IE_FLAG (mstatus image)
                uint tvec    = tf[1];                  // +0x004  TRAP_VECTOR
                uint ieMask  = tf[2];                  // +0x008  IE_MASK
                uint epc     = tf[0x100/4 + 0];        // landing pad word[0] = epc
                uint cause   = tf[0x100/4 + 34];       // landing pad word[34] = cause
                uint tval    = tf[0x100/4 + 33];       // landing pad word[33] = tval (badaddr)
                Console.Error.WriteLine(
                    $"[trace] mtime={emu.MTime:N0} pc=0x{emu.PC:X8} " +
                    $"tvec=0x{tvec:X8} ieflag=0x{ieFlag:X} iemask=0x{ieMask:X} " +
                    $"last_epc=0x{epc:X8} cause=0x{cause:X} tval=0x{tval:X8}");
            }
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
// Restore the console input mode so the user's terminal isn't left in raw mode.
ConsoleHelper.RestoreInput();
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
    Console.Error.WriteLine("  --gui             Start SDL framebuffer/input/audio frontend.");
    Console.Error.WriteLine("  --no-gui          Disable auto GUI for prepared Build_RV32i images.");
    Console.Error.WriteLine("  --auto-commands <commands>");
    Console.Error.WriteLine("                    Send commands at the first BusyBox prompt; semicolons are OK.");
    Console.Error.WriteLine("  --halt-on-output <token>");
    Console.Error.WriteLine("                    Stop the emulator as soon as UART output contains token.");
}

static byte NormalizeGuestInputByte(byte b) => b == 13 ? (byte)10 : b; // CR -> LF

static void ReplaceAscii(byte[] bytes, string oldValue, string newValue)
{
    if (oldValue.Length != newValue.Length)
        throw new ArgumentException("DTB in-place replacements must keep the same length.");

    byte[] oldBytes = System.Text.Encoding.ASCII.GetBytes(oldValue);
    byte[] newBytes = System.Text.Encoding.ASCII.GetBytes(newValue);
    for (int i = 0; i <= bytes.Length - oldBytes.Length; i++)
    {
        bool match = true;
        for (int j = 0; j < oldBytes.Length; j++)
        {
            if (bytes[i + j] == oldBytes[j]) continue;
            match = false;
            break;
        }
        if (!match) continue;

        Array.Copy(newBytes, 0, bytes, i, newBytes.Length);
        return;
    }
}

static void EnqueueGuestInput(UartDevice uart, string commands)
{
    foreach (char ch in commands)
        uart.EnqueueInput(NormalizeGuestInputByte((byte)ch));
    if (!commands.EndsWith('\n') && !commands.EndsWith('\r'))
        uart.EnqueueInput(10);
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

