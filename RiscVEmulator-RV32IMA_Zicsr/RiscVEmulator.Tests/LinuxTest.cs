using System.Diagnostics;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Networking;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Integration tests that boot the real RV32 Linux kernel and assert on the
/// boot console. The kernel + DTB come from Examples.Linux.Build_RV32i
/// (~/.cache/riscvemu/linux/Image-net) or the downloaded mini-rv32ima image.
///
/// These are heavyweight — each test steps the CPU through a full kernel
/// boot (tens of seconds). If no prepared image is present the tests report
/// Inconclusive rather than failing; build one with
/// `dotnet run --project Examples\Linux.Build_RV32i`.
///
/// Filter them out of a fast run with:  dotnet test --filter TestCategory!=Linux
/// </summary>
[TestClass]
public class LinuxTest
{
    // mini-rv32ima machine layout — must match Examples/Linux/Program.cs.
    const uint RamBase    = 0x80000000u;
    const int  RamMB      = 96;
    const uint FbBase     = 0x85C00000u;
    const uint FbSize     = 0x00400000u;       // 4 MB
    const int  FbWidth    = 1024;
    const int  FbHeight   = 768;
    const uint TimebaseHz = 60_000_000u;

    // busybox shell prompt on the serial console.
    const string Prompt = "~ #";

    static string CacheDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".cache", "riscvemu", "linux");

    /// <summary>
    /// Boot the Linux kernel headlessly. Steps the CPU in batches; once
    /// <paramref name="typeAfter"/> has appeared on the console (default: the
    /// shell prompt) and <paramref name="commands"/> is non-null, the commands
    /// are typed into the UART. Stops when <paramref name="stopMarker"/>
    /// appears, the guest halts, or the step / wall-clock budget runs out.
    /// Returns the console text.
    /// </summary>
    static string BootLinux(string stopMarker,
                            string[]? commands = null,
                            string? typeAfter = null,
                            long stepBudget = 8_000_000_000L,
                            int timeoutSeconds = 300)
    {
        string netKernel = Path.Combine(CacheDir, "Image-net");
        string netDtb    = Path.Combine(CacheDir, "rvemu-net.dtb");
        string stdKernel = Path.Combine(CacheDir, "Image");
        string stdDtb    = Path.Combine(CacheDir, "sixtyfourmb.dtb");

        string kernelPath, dtbPath;
        if (File.Exists(netKernel) && File.Exists(netDtb))
            { kernelPath = netKernel; dtbPath = netDtb; }
        else if (File.Exists(stdKernel) && File.Exists(stdDtb))
            { kernelPath = stdKernel; dtbPath = stdDtb; }
        else
        {
            Assert.Inconclusive(
                "No prepared Linux image under ~/.cache/riscvemu/linux/. " +
                "Build one: dotnet run --project Examples\\Linux.Build_RV32i");
            return "";
        }

        bool enableNet = kernelPath == netKernel;
        int  ramSize   = RamMB * 1024 * 1024;

        byte[] kernelImage = File.ReadAllBytes(kernelPath);
        byte[] dtb         = File.ReadAllBytes(dtbPath);

        // Patch the DTB the same way Examples/Linux does: replace the RAM-size
        // guard word with the usable RAM (minus the framebuffer carve-out) and
        // the 1 MHz timebase placeholder with the value the native mtime uses.
        PatchDtbWord(dtb, 0x00C0FF03u, (uint)ramSize - FbSize);
        PatchDtbWord(dtb, 0x000F4240u, TimebaseHz);
        uint dtbRamOffset = (FbBase - RamBase) - (uint)dtb.Length - 64;

        // ── Build the SoC (mirrors Examples/Linux/Program.cs) ──
        var memory = new Memory(ramSize, RamBase);
        var bus    = new MemoryBus(memory);
        var uart   = new UartDevice();
        var syscon = new SysconDevice(0x1110_0000u);
        var clint  = new ClintDevice (0x1100_0000u);
        bus.RegisterPeripheral(uart);
        bus.RegisterPeripheral(syscon);
        bus.RegisterPeripheral(clint);

        if (enableNet)
        {
            var plic = new PlicDevice(0x0C00_0000);
            INetBackend backend = SlirpBridgeBackend.IsAvailable()
                ? new SlirpBridgeBackend()
                : new Win32NatBackend();
            var virtNet = new VirtioNetDevice(0x1000_8000, plic, irqNum: 1,
                                              backend, memory.Reservation.Base);
            bus.RegisterPeripheral(plic);
            bus.RegisterPeripheral(virtNet);
        }

        bus.RegisterPeripheral(new FramebufferDevice(FbWidth, FbHeight, FbBase));
        bus.RegisterPeripheral(new KeyboardDevice());
        bus.RegisterPeripheral(new MouseDevice());
        bus.RegisterPeripheral(new AudioBufferDevice());
        bus.RegisterPeripheral(new AudioControlDevice());
        bus.RegisterPeripheral(new MidiDevice());

        bus.Load(RamBase, kernelImage, 0, kernelImage.Length);
        bus.Load(RamBase + dtbRamOffset, dtb, 0, dtb.Length);

        var regs = new RegisterFile();
        regs.Write(10, 0);                       // a0 = hart id
        regs.Write(11, RamBase + dtbRamOffset);  // a1 = physical DTB address

        using var emu = new Emulator(bus, regs, RamBase);
        emu.RamOffset      = RamBase;
        emu.EnablePrivMode = true;

        var sb = new StringBuilder();
        uart.OutputHandler = c => { lock (sb) sb.Append(c); };
        syscon.OnPowerOff = () => emu.SetHalted(true);
        syscon.OnReboot   = () => emu.SetHalted(true);

        // Step in batches. Once the shell prompt is up, type the commands;
        // stop as soon as the marker shows up so a healthy run finishes fast.
        var sw = Stopwatch.StartNew();
        const int batch = 1_000_000;
        long spent = 0;
        bool typed = commands == null;
        while (spent < stepBudget && !emu.IsHalted &&
               sw.Elapsed.TotalSeconds < timeoutSeconds)
        {
            emu.StepN(batch);
            spent += batch;
            string cur;
            lock (sb) cur = sb.ToString();

            if (!typed && cur.Contains(typeAfter ?? Prompt))
            {
                foreach (string cmd in commands!)
                {
                    foreach (char ch in cmd) uart.EnqueueInput((byte)ch);
                    uart.EnqueueInput((byte)'\n');
                }
                typed = true;
            }
            if (typed && cur.Contains(stopMarker)) break;
        }
        lock (sb) return sb.ToString();
    }

    /// <summary>Replace the first big-endian occurrence of <paramref name="find"/>.</summary>
    static void PatchDtbWord(byte[] dtb, uint find, uint replace)
    {
        for (int o = 0; o + 4 <= dtb.Length; o++)
        {
            uint w = (uint)(dtb[o] << 24 | dtb[o + 1] << 16 |
                            dtb[o + 2] << 8 | dtb[o + 3]);
            if (w != find) continue;
            dtb[o + 0] = (byte)(replace >> 24);
            dtb[o + 1] = (byte)(replace >> 16);
            dtb[o + 2] = (byte)(replace >>  8);
            dtb[o + 3] = (byte)(replace >>  0);
            return;
        }
    }

    static string Tail(string s, int chars = 2500)
        => s.Length <= chars ? s : "…" + s[^chars..];

    // ── Tests ────────────────────────────────────────────────────────────────

    /// <summary>The kernel boots and hands off to the userspace init process.</summary>
    [TestMethod]
    [TestCategory("Linux")]
    public void Linux_BootsToUserspace()
    {
        string log = BootLinux(stopMarker: "Run /init");

        StringAssert.Contains(log, "Linux version",
            "Kernel never printed its version banner.\n--- console ---\n" + Tail(log));
        Assert.IsFalse(log.Contains("Kernel panic"),
            "Kernel panicked during boot.\n--- console ---\n" + Tail(log));
        Assert.IsFalse(log.Contains("unhandled signal 4"),
            "An illegal-instruction trap occurred during boot.\n--- console ---\n" + Tail(log));
        StringAssert.Contains(log, "Run /init",
            "Kernel never reached the userspace init process.\n--- console ---\n" + Tail(log));
    }

    /// <summary>
    /// The guest's network stack initialises: the boot reaches the network
    /// init script and it does not report failure.
    /// </summary>
    [TestMethod]
    [TestCategory("Linux")]
    public void Linux_NetworkStackComesUp()
    {
        string log = BootLinux(stopMarker: "Starting network: OK");

        if (!log.Contains("Linux version"))
            Assert.Inconclusive("Kernel did not boot at all — see Linux_BootsToUserspace.");
        if (!log.Contains("Starting network"))
            Assert.Inconclusive(
                "This image has no network init (downloaded image, not the " +
                "Build_RV32i networked build).\n--- console ---\n" + Tail(log));

        Assert.IsFalse(log.Contains("Kernel panic"),
            "Kernel panicked during boot.\n--- console ---\n" + Tail(log));
        Assert.IsFalse(log.Contains("Starting network: FAIL"),
            "Network init script reported FAIL.\n--- console ---\n" + Tail(log));
        StringAssert.Contains(log, "Starting network: OK",
            "Network init did not complete successfully.\n--- console ---\n" + Tail(log));
    }

    /// <summary>
    /// End-to-end slirp test: boot, then `ping` the libslirp NAT gateway
    /// (10.0.2.2) from the guest shell and confirm ICMP echo round-trips.
    /// </summary>
    [TestMethod]
    [TestCategory("Linux")]
    public void Linux_SlirpPingWorks()
    {
        if (!SlirpBridgeBackend.IsAvailable())
            Assert.Inconclusive("slirp_bridge.dll not available — libslirp NAT (and ICMP) is off.");

        // At the shell prompt, give DHCP a moment to lease eth0, then ping the
        // libslirp NAT gateway (10.0.2.2). Self-contained so it does not
        // depend on boot-log timing; stops on ping's own statistics line.
        string log = BootLinux(
            stopMarker: "packets transmitted",
            commands:   new[] { "sleep 12; ping -c 3 -W 2 10.0.2.2" });

        if (!log.Contains("Linux version"))
            Assert.Inconclusive("Kernel did not boot — see Linux_BootsToUserspace.");
        if (!log.Contains(Prompt))
            Assert.Inconclusive("Guest never reached a shell prompt.\n--- console ---\n" + Tail(log));

        StringAssert.Contains(log, "packets transmitted",
            "ping did not run / produced no statistics.\n--- console ---\n" + Tail(log));
        Assert.IsFalse(log.Contains("bad address"),
            "ping could not resolve the target.\n--- console ---\n" + Tail(log));
        Assert.IsFalse(log.Contains("100% packet loss"),
            "All ICMP echoes were lost — slirp is not forwarding ping.\n--- console ---\n" + Tail(log));
        StringAssert.Contains(log, "bytes from",
            "No ICMP echo reply received from the slirp gateway.\n--- console ---\n" + Tail(log));
    }
}
