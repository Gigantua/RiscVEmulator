using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Drives the Quake bare-metal ELF far enough to confirm that the
/// `+map e1m1` shortcut in `quake_main.c` lands the player in the
/// real game world. This is the closest thing to an end-to-end test
/// the emulator has — it exercises the JIT, DiskDevice paging,
/// softfloat, MMIO probe routing, and trap-frame handling all at
/// once.
///
/// Skipped if the prebuilt `quake.elf` or `pak0.pak` are absent —
/// running the full test only makes sense when the Examples/Quake
/// project has been built at least once (which produces both
/// artefacts under `Examples/Quake/bin/.../build` and `id1/`).
/// </summary>
[TestClass]
public class QuakeBootTest
{
    public TestContext? TestContext { get; set; }

    /// <summary>Locate Examples/Quake artefacts relative to the test exe.</summary>
    private static (string elf, string pak) ResolveArtefacts()
    {
        // Walk upward from the test exe directory looking for the
        // Examples folder; that locates the solution root regardless of
        // bin/Debug vs bin/x64/Debug nesting depth.
        string dir = AppContext.BaseDirectory;
        string? solnRoot = null;
        for (var d = new DirectoryInfo(dir); d != null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "Examples", "Quake")))
            { solnRoot = d.FullName; break; }
        solnRoot ??= dir;

        string[] elfCandidates =
        {
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "x64", "Release", "net10.0", "build", "quake.elf"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "Release", "net10.0", "build", "quake.elf"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "x64", "Debug", "net10.0", "build", "quake.elf"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "Debug", "net10.0", "build", "quake.elf"),
        };
        string[] pakCandidates =
        {
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "x64", "Release", "net10.0", "id1", "pak0.pak"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "Release", "net10.0", "id1", "pak0.pak"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "x64", "Debug", "net10.0", "id1", "pak0.pak"),
            Path.Combine(solnRoot, "Examples", "Quake", "bin", "Debug", "net10.0", "id1", "pak0.pak"),
            Path.Combine(solnRoot, "Examples", "Quake", "id1", "pak0.pak"),
        };
        string elf = elfCandidates.FirstOrDefault(File.Exists) ?? elfCandidates[0];
        string pak = pakCandidates.FirstOrDefault(File.Exists) ?? pakCandidates[0];
        return (elf, pak);
    }

    /// <summary>
    /// Spins up the same SoC Examples/Quake uses, loads the prebuilt
    /// quake.elf + pak0.pak, and runs the emulator. Returns the
    /// peripherals and stdout buffer for post-run inspection by each
    /// test method below.
    /// </summary>
    private sealed record QuakeRun(
        string Output, ulong VsyncCount, ulong VsyncAtSpawn,
        byte[] PresentedPixels, bool Halted, long Stepped);

    private static QuakeRun RunQuake(long budgetSteps, string? earlyExitKey = null,
                                     ulong inGameVsyncTarget = 0)
    {
        var (elfPath, pakPath) = ResolveArtefacts();
        if (!File.Exists(elfPath) || !File.Exists(pakPath))
            Assert.Inconclusive(
                $"Quake artefacts not built. Build Examples.Quake first.\n  elf: {elfPath}\n  pak: {pakPath}");

        // 64 MiB RAM to match Examples/Quake/Program.cs.
        var memory  = new Memory(64 * 1024 * 1024);
        var bus     = new MemoryBus(memory);
        var uart    = new UartDevice();
        var fb      = new FramebufferDevice();
        var display = new DisplayControlDevice(fb);
        display.SetMemory(memory);
        var kbd       = new KeyboardDevice();
        var mouse     = new MouseDevice();
        var rtc       = new RealTimeClockDevice();
        var audioBuf  = new AudioBufferDevice();
        var audioCtrl = new AudioControlDevice();
        var clint     = new ClintDevice();
        var trapFrame = new TrapFrameDevice();
        var disk      = new DiskDevice();
        disk.LoadFile(File.ReadAllBytes(pakPath), bus);
        bus.RegisterPeripheral(uart);
        bus.RegisterPeripheral(fb);
        bus.RegisterPeripheral(display);
        bus.RegisterPeripheral(kbd);
        bus.RegisterPeripheral(mouse);
        bus.RegisterPeripheral(rtc);
        bus.RegisterPeripheral(audioBuf);
        bus.RegisterPeripheral(audioCtrl);
        bus.RegisterPeripheral(clint);
        bus.RegisterPeripheral(trapFrame);
        bus.RegisterPeripheral(disk);

        byte[] elfData = File.ReadAllBytes(elfPath);
        var regs       = new RegisterFile();
        uint entry     = ElfLoader.Load(elfData, bus);
        regs.Write(2, 0x03FFFFF0u);

        var sb = new StringBuilder();
        var emu = new Emulator(bus, regs, entry);
        emu.OutputHandler  = c => sb.Append(c);
        uart.OutputHandler = c => sb.Append(c);

        const int BatchSteps = 100_000_000;
        const string SpawnKey = "player entered the game";
        long stepped = 0;
        ulong vsyncAtSpawn = 0;
        bool spawnSeen = false;
        while (stepped < budgetSteps)
        {
            emu.Run(BatchSteps);
            stepped += BatchSteps;
            if (!spawnSeen && sb.ToString().Contains(SpawnKey))
            {
                spawnSeen = true;
                vsyncAtSpawn = display.VsyncCount;
            }
            if (emu.IsHalted) break;
            if (earlyExitKey != null && sb.ToString().Contains(earlyExitKey)) break;
            if (inGameVsyncTarget > 0 && spawnSeen &&
                display.VsyncCount - vsyncAtSpawn >= inGameVsyncTarget) break;
        }

        return new QuakeRun(sb.ToString(), display.VsyncCount, vsyncAtSpawn,
                            (byte[])fb.PresentedPixels.Clone(),
                            emu.IsHalted, stepped);
    }

    /// <summary>
    /// Fast boot-only smoke test — confirms Quake reaches ca_active
    /// (signon=4) without exercising the renderer beyond the title
    /// frames. Cheap to run on every commit.
    /// </summary>
    [TestMethod]
    [TestCategory("LongRunning")]
    public void EntersE1M1WithinBudget()
    {
        var run = RunQuake(budgetSteps: 40_000_000_000L,
                           earlyExitKey: "player entered the game");
        Assert.IsTrue(run.Output.Contains("player entered the game"),
            $"Quake did not reach in-game state within {run.Stepped:N0} steps.\n" +
            $"--- last 2 KiB of stdout ---\n" +
            run.Output[Math.Max(0, run.Output.Length - 2048)..]);
    }

    /// <summary>
    /// Drives Quake far past ca_active and verifies the software
    /// renderer is producing frames. Counts the VSYNC=1 presents and
    /// snapshots the framebuffer; a frozen renderer either fails to
    /// produce any presents or leaves the FB at all-zeroes.
    ///
    /// Budget is large (250 B steps ≈ 4 min on Debug, ~30 s on Release)
    /// because softfloat math through the BSP traversal + lightmap
    /// generation makes the first in-game frame very slow on rv32i.
    /// </summary>
    [TestMethod]
    [TestCategory("LongRunning")]
    public void RendersManyFrames()
    {
        // 600 B steps ~= 9 min Debug / 6 min Release. First in-game
        // frame is ~200 B steps (BSP traversal + lightmap cache fill
        // through softfloat); each subsequent frame is far cheaper
        // once the surface cache is warm. Bail out early once we've
        // proven the renderer can produce frames repeatedly.
        var run = RunQuake(budgetSteps: 600_000_000_000L, inGameVsyncTarget: 3);

        // Hard floor: we must at least clear the very first signon
        // hand-shake. If this fails the engine is wedged before boot.
        Assert.IsTrue(run.Output.Contains("player entered the game"),
            $"never reached ca_active in {run.Stepped:N0} steps.\n" +
            $"--- last 2 KiB of stdout ---\n" +
            run.Output[Math.Max(0, run.Output.Length - 2048)..]);

        // Renderer liveness AFTER the in-game spawn. The boot/menu
        // path also calls VID_Update so VsyncCount alone is satisfied
        // by ~20 console-only frames before ca_active — that's NOT
        // proof the 3D renderer works. What matters: presents AFTER
        // "player entered the game" was printed.
        ulong inGameVsyncs = run.VsyncCount - run.VsyncAtSpawn;
        TestContext?.WriteLine(
            $"vsyncs total={run.VsyncCount} at-spawn={run.VsyncAtSpawn} " +
            $"post-spawn={inGameVsyncs} steps={run.Stepped:N0}");
        Assert.IsTrue(inGameVsyncs >= 3,
            $"only {inGameVsyncs} in-game frames presented (total {run.VsyncCount}, " +
            $"{run.VsyncAtSpawn} pre-spawn) in {run.Stepped:N0} steps — renderer " +
            $"is stuck (likely R_ScanEdges / softfloat path).");

        // Pixel content: the FB has 320×200×4 = 256 000 bytes. If the
        // renderer ran but produced an all-black frame, *some* pixels
        // should still differ from the boot-time zero-fill (Quake's
        // status bar alone has hundreds of non-zero pixels).
        int nonZeroBytes = 0;
        foreach (byte b in run.PresentedPixels)
            if (b != 0 && b != 0xFF /* opaque-alpha-only counts as zero */) nonZeroBytes++;
        Assert.IsTrue(nonZeroBytes > 1000,
            $"only {nonZeroBytes} non-trivial bytes in the 256 KiB framebuffer " +
            $"after {run.VsyncCount} presents — renderer ran but drew nothing.");

        // Dump the final framebuffer as a 320×200 BMP next to the test
        // exe so a human can inspect what Quake actually drew. BMP keeps
        // the test self-contained (no PNG dependency).
        string bmpPath = Path.Combine(AppContext.BaseDirectory,
                                      "quake_final_frame.bmp");
        WriteBmp(bmpPath, 320, 200, run.PresentedPixels);
        TestContext?.WriteLine(
            $"vsync presents: {run.VsyncCount} | non-zero bytes: {nonZeroBytes} | " +
            $"FB snapshot: {bmpPath}");
    }

    /// <summary>
    /// 32-bit RGBA → 24-bit BGR BMP. Top-down row order (negative
    /// height) so writing rows in memory order Just Works.
    /// </summary>
    private static void WriteBmp(string path, int width, int height, byte[] rgba)
    {
        int rowBytes = (width * 3 + 3) & ~3;
        int pixelBytes = rowBytes * height;
        int fileSize = 54 + pixelBytes;
        using var fs = File.Create(path);
        using var w  = new BinaryWriter(fs);
        // BMP header
        w.Write((ushort)0x4D42);          // "BM"
        w.Write(fileSize);
        w.Write((uint)0);
        w.Write((uint)54);                // pixel offset
        // DIB header
        w.Write((uint)40);
        w.Write(width);
        w.Write(-height);                 // top-down
        w.Write((ushort)1);
        w.Write((ushort)24);
        w.Write((uint)0);                 // BI_RGB
        w.Write((uint)pixelBytes);
        w.Write(0); w.Write(0); w.Write((uint)0); w.Write((uint)0);
        byte[] row = new byte[rowBytes];
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int src = (y * width + x) * 4;
                row[x * 3 + 0] = rgba[src + 2]; // B
                row[x * 3 + 1] = rgba[src + 1]; // G
                row[x * 3 + 2] = rgba[src + 0]; // R
            }
            w.Write(row);
        }
    }
}
