using RiscVEmulator.Core;
using RiscVEmulator.Core.Networking;
using RiscVEmulator.Core.Peripherals;

namespace Examples.Linux;

/// <summary>
/// Boots a real rv64gc Linux distribution — an Alpine <c>riscv64</c> rootfs
/// supplied as an external initramfs — on the 64-bit core
/// (<see cref="Emulator64"/> → <c>rv64gc_core.cpp</c>). The emulator stands
/// in for the M-mode SBI firmware: the kernel is entered directly in S-mode
/// and its SBI ecalls are serviced inside the native core.
///
/// Reached via <c>Examples.Linux --rv64</c>. RAM is a fixed 1 GiB; the last
/// 4 MiB is the framebuffer. A PLIC + virtio-net are always wired (the guest
/// uses them for <c>apk</c>); <c>--gui</c> additionally opens an SDL window
/// and tells the guest /init to launch the desktop.
///
/// Three little-endian u32 guard words in the DTB are patched at load time:
///   0xC0FFEE00 → /memory size, 0xC0FFEE01/02 → initrd start/end.
/// </summary>
internal static class Rv64Boot
{
    private const uint RamBase = 0x80000000u;
    private const int  RamMB   = 1024;                            // fixed → fixed FbBase

    // GUI framebuffer — the last 4 MiB of the RAM bank. /memory is patched to
    // stop at FbBase so the kernel never allocates these pages; the host
    // FramebufferDevice + SDL viewer read them live, the guest mmaps /dev/mem
    // (or the kernel's simplefb exposes /dev/fb0) here.
    private const uint FbSize = 0x400000u;
    private const uint FbBase = RamBase + (uint)RamMB * 1024 * 1024 - FbSize;   // 0xBFC00000
    private const int  FbW    = 1024;
    private const int  FbH    = 768;

    /// <summary>Replace the first big-endian u32 equal to <paramref name="magic"/>
    /// with <paramref name="value"/> (also big-endian). Returns false if absent.</summary>
    private static bool PatchBE32(byte[] buf, uint magic, uint value)
    {
        for (int o = 0; o + 4 <= buf.Length; o++)
        {
            if (buf[o]   == (byte)(magic >> 24) && buf[o+1] == (byte)(magic >> 16) &&
                buf[o+2] == (byte)(magic >>  8) && buf[o+3] == (byte) magic)
            {
                buf[o]   = (byte)(value >> 24); buf[o+1] = (byte)(value >> 16);
                buf[o+2] = (byte)(value >>  8); buf[o+3] = (byte) value;
                return true;
            }
        }
        return false;
    }

    /// <summary>Overwrite the first ASCII occurrence of <paramref name="find"/>
    /// with <paramref name="repl"/>, space-padded to the original length.</summary>
    private static bool ReplaceAscii(byte[] buf, string find, string repl)
    {
        byte[] f = System.Text.Encoding.ASCII.GetBytes(find);
        for (int i = 0; i + f.Length <= buf.Length; i++)
        {
            bool hit = true;
            for (int j = 0; j < f.Length; j++)
                if (buf[i + j] != f[j]) { hit = false; break; }
            if (!hit) continue;
            for (int j = 0; j < f.Length; j++)
                buf[i + j] = (byte)(j < repl.Length ? repl[j] : ' ');
            return true;
        }
        return false;
    }

    /// <summary>Load a flat RV64GC binary at RamBase and run it bare-metal in
    /// M-mode (no SBI, no kernel). UART output goes to the console; the program
    /// halts by writing its exit code to the host-exit device. Returns that
    /// exit code.</summary>
    private static int RunBareMetal(string binPath)
    {
        if (!File.Exists(binPath))
        {
            Console.Error.WriteLine($"--rv64-bin: file not found: {binPath}");
            return 1;
        }
        byte[] bin     = File.ReadAllBytes(binPath);
        int    ramSize = 64 * 1024 * 1024;

        var memory = new Memory(ramSize, RamBase);
        var bus    = new MemoryBus(memory);
        var uart   = new UartDevice();
        bus.RegisterPeripheral(uart);
        bus.Load(RamBase, bin, 0, bin.Length);

        var emu = new Emulator64(bus, new RegisterFile(), RamBase);  // M-mode, no SBI
        uart.OutputHandler = c => Console.Write(c);

        Console.Error.WriteLine($"[rv64-bin] running {bin.Length:N0} B flat binary at 0x{RamBase:X8}...");
        int batches = 0;
        while (!emu.IsHalted && batches < 4000)
        {
            emu.StepN(200_000);
            batches++;
        }
        Console.Error.WriteLine(emu.IsHalted
            ? $"[rv64-bin] halted — exit code {emu.ExitCode}."
            : "[rv64-bin] TIMEOUT — did not halt.");
        int rc = emu.IsHalted ? emu.ExitCode : 2;
        emu.Dispose();
        return rc;
    }

    public static int Run(string[] args)
    {
        // Bare-metal mode: load a flat RV64GC binary at RamBase, run it in
        // M-mode with no SBI/kernel — used by the ISA self-test.
        for (int k = 0; k + 1 < args.Length; k++)
            if (args[k] == "--rv64-bin")
                return RunBareMetal(args[k + 1]);

        string cacheDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".cache", "riscvemu", "linux");
        string  kernelPath  = Path.Combine(cacheDir, "Image-rv64");
        string  initrdPath  = Path.Combine(cacheDir, "initramfs-rv64.cpio");
        string? dtbOverride = null;
        string? autoCommands = null;
        bool    gui = false;
        string? screenshotPath = null;
        int     shotAfterSec   = 40;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--rv64":                              break;
                case "--gui":     gui = true;               break;
                case "--no-gui":  gui = false;              break;
                case "--kernel":  kernelPath  = args[++i];  break;
                case "--initrd":  initrdPath  = args[++i];  break;
                case "--dtb":     dtbOverride = args[++i];  break;
                case "--auto-commands": autoCommands = args[++i]; break;
                case "--screenshot":    screenshotPath = args[++i]; break;
                case "--shot-after":    shotAfterSec   = int.Parse(args[++i]); break;
                default:
                    Console.Error.WriteLine($"--rv64: unknown option {args[i]}");
                    return 1;
            }
        }
        if (screenshotPath != null) gui = true;     // a screenshot needs the framebuffer

        string dtbPath = dtbOverride ??
            Path.Combine(AppContext.BaseDirectory, "rvemu-rv64.dtb");

        if (!File.Exists(kernelPath))
        {
            Console.Error.WriteLine($"RV64 kernel not found: {kernelPath}");
            Console.Error.WriteLine("Build it via buildroot qemu_riscv64_virt_defconfig, or pass --kernel <path>.");
            return 1;
        }
        if (!File.Exists(dtbPath))
        {
            Console.Error.WriteLine($"DTB not found: {dtbPath} (expected next to the executable).");
            return 1;
        }
        if (!File.Exists(initrdPath))
        {
            Console.Error.WriteLine($"Initramfs not found: {initrdPath}");
            Console.Error.WriteLine("Build it from an Alpine riscv64 minirootfs, or pass --initrd <path>.");
            return 1;
        }

        byte[] kernel  = File.ReadAllBytes(kernelPath);
        byte[] dtb     = File.ReadAllBytes(dtbPath);
        byte[] initrd  = File.ReadAllBytes(initrdPath);
        int    ramSize = RamMB * 1024 * 1024;

        // RISC-V flat Image header: text_offset is a little-endian u64 at byte 8.
        ulong textOffset = kernel.Length >= 16 ? BitConverter.ToUInt64(kernel, 8) : 0;
        if (textOffset == 0 || textOffset >= (ulong)ramSize)
            textOffset = 0x200000;                          // 2 MiB default

        uint loadAddr = RamBase + (uint)textOffset;
        // /memory stops at FbBase, so the DTB + initramfs sit below the FB.
        uint dtbAddr    = FbBase - 0x100000u;
        uint initrdAddr = (dtbAddr - 0x1000000u - (uint)initrd.Length) & ~0xFFFu;
        uint initrdEnd  = initrdAddr + (uint)initrd.Length;
        uint entry      = loadAddr;     // the rv64gc core decodes the Image header natively

        if (loadAddr + (uint)kernel.Length > initrdAddr)
        {
            Console.Error.WriteLine($"Kernel ({kernel.Length:N0} B) at 0x{loadAddr:X8} overruns the initramfs slot.");
            return 1;
        }

        // Patch the DTB guard words: RAM size (shrunk past the FB) + initrd window.
        bool pRam = PatchBE32(dtb, 0xC0FFEE00u, (uint)ramSize - FbSize);
        bool pIs  = PatchBE32(dtb, 0xC0FFEE01u, initrdAddr);
        bool pIe  = PatchBE32(dtb, 0xC0FFEE02u, initrdEnd);
        if (!pRam || !pIs || !pIe)
            Console.Error.WriteLine($"Warning: DTB guard word(s) not found (ram={pRam} initrd-start={pIs} initrd-end={pIe}).");
        // --gui: mark the kernel command line so the guest /init launches the
        // desktop instead of a console shell.
        if (gui && !ReplaceAscii(dtb, "earlycon=sbi", "rvemu_gui"))
            Console.Error.WriteLine("Warning: --gui could not patch bootargs (earlycon=sbi not found).");

        // ── SoC: RAM, ns16550 UART, PLIC, virtio-net, framebuffer, input ──
        var memory = new Memory(ramSize, RamBase);
        var bus    = new MemoryBus(memory);
        var uart   = new UartDevice();
        bus.RegisterPeripheral(uart);

        var plic = new PlicDevice(0x0C00_0000u);
        INetBackend backend = SlirpBridgeBackend.IsAvailable()
            ? new SlirpBridgeBackend()
            : new Win32NatBackend();
        var virtNet = new VirtioNetDevice(0x1000_8000u, plic, irqNum: 1,
                                          backend, memory.Reservation.Base);
        bus.RegisterPeripheral(plic);
        bus.RegisterPeripheral(virtNet);

        // virtio-blk — a persistent 3 GiB disk image (the Alpine + XFCE root).
        string diskPath = Path.Combine(cacheDir, "rv64-disk.img");
        if (!File.Exists(diskPath))
        {
            using var ds = File.Create(diskPath);
            ds.SetLength(3L * 1024 * 1024 * 1024);
        }
        var virtBlk = new VirtioBlkDevice(0x1000_9000u, plic, irqNum: 2,
                                          diskPath, memory.Reservation.Base);
        bus.RegisterPeripheral(virtBlk);

        var framebuffer = new FramebufferDevice(FbW, FbH, FbBase);
        var keyboard    = new KeyboardDevice();
        var mouse       = new MouseDevice();
        bus.RegisterPeripheral(framebuffer);
        bus.RegisterPeripheral(keyboard);
        bus.RegisterPeripheral(mouse);

        // Audio MMIO — PCM ring at 0x30000000, control regs at 0x30100000.
        // The guest writes PCM samples; LinuxSdlAudio (host) drains them to
        // SDL when --gui is set.
        var audioBuf  = new AudioBufferDevice();
        var audioCtrl = new AudioControlDevice();
        bus.RegisterPeripheral(audioBuf);
        bus.RegisterPeripheral(audioCtrl);

        bus.Load(loadAddr,   kernel, 0, kernel.Length);
        bus.Load(initrdAddr, initrd, 0, initrd.Length);
        bus.Load(dtbAddr,    dtb,    0, dtb.Length);

        // RISC-V boot protocol: a0 = hart id, a1 = physical DTB address.
        var regs = new RegisterFile();
        regs.Write(10, 0);
        regs.Write(11, dtbAddr);

        var emu = new Emulator64(bus, regs, entry);
        emu.RamOffset = RamBase;
        emu.EnableSbiMode();          // enter S-mode; emulator services SBI ecalls

        // Route PLIC external IRQs into the 64-bit core.
        PlicDevice.MachineIrqSink    = Emulator64.SetMachineExtIrq;
        PlicDevice.SupervisorIrqSink = Emulator64.SetSupervisorExtIrq;

        ConsoleHelper.EnableVt();

        // UART → console, with an incremental matcher for the sentinel the
        // Alpine /init prints just before exec'ing the shell.
        const string ReadyMarker = "RVEMU_SHELL_READY";
        int  markerPos  = 0;
        bool markerSeen = false;
        long lastUartTick = Environment.TickCount64;
        uart.OutputHandler = c =>
        {
            Console.Write(c);
            if (!markerSeen)
            {
                if (c == ReadyMarker[markerPos])
                {
                    if (++markerPos == ReadyMarker.Length) markerSeen = true;
                }
                else markerPos = (c == ReadyMarker[0]) ? 1 : 0;
            }
            lastUartTick = Environment.TickCount64;
        };

        var cts = new System.Threading.CancellationTokenSource();
        try { Console.TreatControlCAsInput = true; } catch { /* not a console */ }

        var stdinThread = new System.Threading.Thread(() =>
        {
            try
            {
                var stdin = Console.OpenStandardInput();
                var buf   = new byte[64];
                int n;
                while (!cts.IsCancellationRequested && (n = stdin.Read(buf, 0, buf.Length)) > 0)
                    for (int j = 0; j < n; j++)
                        uart.EnqueueInput(buf[j] == 13 ? (byte)10 : buf[j]);   // CR→LF
            }
            catch { /* stdin closed */ }
        }) { IsBackground = true, Name = "StdinReader" };
        stdinThread.Start();

        // Non-interactive smoke path — wait for the /init sentinel, inject once.
        if (autoCommands != null)
            new System.Threading.Thread(() =>
            {
                // Fire on the initramfs sentinel, or — for the booted-from-disk
                // system, which prints no sentinel and ends its prompt with a
                // \e[6n escape — once the UART has fallen quiet (a login shell
                // waiting for input). Injecting a touch early is harmless: the
                // bytes queue in the UART and run when the shell reads them.
                while (!cts.IsCancellationRequested && !emu.IsHalted)
                {
                    System.Threading.Thread.Sleep(150);
                    // The sentinel (printed by the initramfs and by the disk's
                    // /root/.profile) is the reliable trigger; the long idle is
                    // only a last-resort fallback that can't fire on a boot gap.
                    if (markerSeen || Environment.TickCount64 - lastUartTick > 25000)
                        break;
                }
                if (cts.IsCancellationRequested || emu.IsHalted) return;
                System.Threading.Thread.Sleep(400);
                foreach (char ch in autoCommands!)
                    uart.EnqueueInput((byte)(ch == '\r' ? '\n' : ch));
                uart.EnqueueInput(10);
                Console.Error.WriteLine($"[auto-commands] injected: {autoCommands}");
            }) { IsBackground = true, Name = "AutoCommands" }.Start();

        Console.Error.WriteLine($"Booting RV64GC Linux: kernel {kernel.Length:N0} B @ 0x{loadAddr:X8}, " +
                                $"initramfs {initrd.Length:N0} B @ 0x{initrdAddr:X8}, DTB @ 0x{dtbAddr:X8}, " +
                                $"{RamMB} MB RAM{(gui ? ", GUI" : "")}.");
        Console.Error.WriteLine("Emulator is acting as SBI firmware; kernel runs in S-mode. Ctrl+C to exit.\n");

        // ── GUI: SDL window — renders the FB, forwards host keyboard/mouse ──
        LinuxSdlViewer? viewer = null;
        LinuxSdlAudio?  audio  = null;
        if (gui)
        {
            viewer = new LinuxSdlViewer(framebuffer, keyboard, mouse,
                                        title: "rvemu RV64 — Alpine desktop", scale: 1);
            viewer.Start();
            audio = new LinuxSdlAudio(audioBuf, audioCtrl);
            audio.Start();
            Console.Error.WriteLine("SDL viewer + audio started (--gui).");
        }

        if (Environment.GetEnvironmentVariable("RVEMU_TRACE") == "1")
            new System.Threading.Thread(() =>
            {
                while (!cts.IsCancellationRequested && !emu.IsHalted)
                {
                    System.Threading.Thread.Sleep(2000);
                    if (cts.IsCancellationRequested || emu.IsHalted) break;
                    Console.Error.WriteLine(
                        $"[trace] pc=0x{emu.PC:X16} priv={emu.PrivMode} " +
                        $"satp=0x{emu.Dbg(8):X} traps={emu.Dbg(7)} sbi={emu.Dbg(9)} scause=0x{emu.Dbg(3):X}");
                }
            }) { IsBackground = true, Name = "Heartbeat" }.Start();

        // Headless screenshot: boot, let the desktop render, dump the FB to PNG.
        if (screenshotPath != null)
            new System.Threading.Thread(() =>
            {
                System.Threading.Thread.Sleep(shotAfterSec * 1000);
                try
                {
                    PngWriter.Write(screenshotPath, framebuffer.Pixels, framebuffer.Width, framebuffer.Height);
                    Console.Error.WriteLine($"Screenshot written: {screenshotPath}");
                }
                catch (Exception ex) { Console.Error.WriteLine($"Screenshot failed: {ex.Message}"); }
                cts.Cancel();
            }) { IsBackground = true, Name = "Screenshot" }.Start();

        const int BatchSize = 500_000;
        while (!emu.IsHalted && !cts.IsCancellationRequested)
            emu.StepN(BatchSize);

        cts.Cancel();
        viewer?.Stop();
        audio?.Stop();
        Console.Error.WriteLine($"\nEmulator stopped. Executed ~{emu.MTime:N0} ticks.");
        if (Environment.GetEnvironmentVariable("RVEMU_PRIV_TRACE") == "1")
            Console.Error.WriteLine($"[priv-trace] priv-mode={emu.PrivMode} pc=0x{emu.PC:X16} " +
                                    $"traps={emu.Dbg(7)} sbi-calls={emu.Dbg(9)}");
        return 0;
    }
}
