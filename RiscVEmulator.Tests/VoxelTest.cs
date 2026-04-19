using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Step-by-step tests for the Voxel renderer.
/// Each test isolates one layer of the pipeline to make failures easy to locate:
///   1. MkRgba   — pixel packing byte order
///   2. ClearScreen — sky gradient writes to FB
///   3. Triangle — rasterizer draws a triangle
///   4. Render   — full pipeline: MVP → vertex → fragment → FB
/// All tests compile with -march=rv32im and run with M-extension enabled.
/// </summary>
[TestClass]
public class VoxelTest : EmulatorTestBase
{
    /* Path to voxel_main.c — walk up from output dir until we find Examples/Voxel/Programs */
    private static readonly string VoxelProgramsDir = FindVoxelProgramsDir();

    private static string FindVoxelProgramsDir()
    {
        string? dir = AppContext.BaseDirectory;
        while (dir != null)
        {
            string candidate = Path.Combine(dir, "Examples", "Voxel", "Programs");
            if (Directory.Exists(candidate))
                return Path.GetFullPath(candidate);
            dir = Path.GetDirectoryName(dir);
        }
        throw new DirectoryNotFoundException(
            $"Could not find Examples/Voxel/Programs starting from {AppContext.BaseDirectory}");
    }

    // ── Compile helpers ──────────────────────────────────────────────────────

    /// <summary>
    /// Compile one or more sources to an ELF using rv32im (M-extension enabled).
    /// Adds -I{VoxelProgramsDir} and -I{RuntimeDir} so test C files can
    /// <c>#include "voxel_main.c"</c>.
    /// </summary>
    private static void CompileMExt(IEnumerable<string> srcFiles, string elfFile)
    {
        var psi = new ProcessStartInfo("clang")
        {
            WorkingDirectory       = TestDir,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            UseShellExecute        = false,
        };
        psi.ArgumentList.Add("--target=riscv32-unknown-elf");
        psi.ArgumentList.Add("-march=rv32im");
        psi.ArgumentList.Add("-mabi=ilp32");
        psi.ArgumentList.Add("-nostdlib");
        psi.ArgumentList.Add("-nostartfiles");
        psi.ArgumentList.Add("-O3");
        psi.ArgumentList.Add("-fno-builtin");
        psi.ArgumentList.Add("-fsigned-char");
        psi.ArgumentList.Add("-fuse-ld=lld");
        psi.ArgumentList.Add($"-I{VoxelProgramsDir}");
        psi.ArgumentList.Add($"-I{RuntimeDir}");
        psi.ArgumentList.Add($"-Wl,-T,{LinkerScript}");
        foreach (string src in srcFiles)
            psi.ArgumentList.Add(src);
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add(elfFile);

        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to start clang.");
        string stdout = proc.StandardOutput.ReadToEnd();
        string stderr = proc.StandardError.ReadToEnd();
        proc.WaitForExit();

        Assert.IsTrue(proc.ExitCode == 0,
            $"clang failed (exit {proc.ExitCode}).\nstdout: {stdout}\nstderr: {stderr}");
    }

    /// <summary>Compile craft test + full runtime (including math.c) and run.</summary>
    private static RunResult CompileAndRun(string testProgram, int maxSteps = 100_000_000)
    {
        string srcFile = Path.Combine(ProgramDir, $"{testProgram}.c");
        string elfFile = Path.Combine(TestDir, $"{testProgram}.elf");

        Assert.IsTrue(File.Exists(srcFile), $"Test source not found: {srcFile}");

        var sources = new List<string>
        {
            Path.Combine(RuntimeDir, "crt0.c"),
            Path.Combine(RuntimeDir, "runtime.c"),
            Path.Combine(RuntimeDir, "softfloat.c"),
            Path.Combine(RuntimeDir, "math.c"),
            Path.Combine(RuntimeDir, "libc.c"),
            Path.Combine(RuntimeDir, "syscalls.c"),
            Path.Combine(RuntimeDir, "malloc.c"),
            Path.Combine(RuntimeDir, "vfs.c"),
            srcFile,
        };

        CompileMExt(sources, elfFile);
        return RunElfFull(elfFile, maxSteps, enableMExtension: true);
    }

    // ── Test 1: mk_rgba pixel encoding ──────────────────────────────────────

    [TestMethod]
    public void VoxelMkRgba_ByteOrderIsRGBA8888()
    {
        var result = CompileAndRun("voxel_mkrgba_test");
        Assert.IsTrue(result.Halted,    $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "red_word: OK",     result.Output);
        StringAssert.Contains(result.Output, "red_byte0_R: OK",  result.Output);
        StringAssert.Contains(result.Output, "red_byte1_G: OK",  result.Output);
        StringAssert.Contains(result.Output, "red_byte2_B: OK",  result.Output);
        StringAssert.Contains(result.Output, "green_word: OK",   result.Output);
        StringAssert.Contains(result.Output, "blue_word: OK",    result.Output);
    }

    // ── Test 2: clear_screen writes non-black sky gradient ──────────────────

    [TestMethod]
    public void VoxelClearScreen_WritesSkyGradientToFramebuffer()
    {
        var result = CompileAndRun("voxel_clearscreen_test");
        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "nonblack: OK",  result.Output);
        StringAssert.Contains(result.Output, "gradient: OK",  result.Output);
        StringAssert.Contains(result.Output, "alpha_ok: OK",  result.Output);
        StringAssert.Contains(result.Output, "depth_ok: OK",  result.Output);

        // Also verify via framebuffer peripheral: pixel (0,0) should have
        // R != 0 (sky is not black at the top)
        byte[] px = result.Framebuffer.Pixels;
        Assert.AreNotEqual(0, px[0] | px[1] | px[2],
            "Pixel (0,0) is black — clear_screen did not write FB");
    }

    // ── Test 3: rasterizer draws a triangle ─────────────────────────────────

    [TestMethod]
    public void VoxelTriangle_DrawsPixelsInBoundingBox()
    {
        var result = CompileAndRun("voxel_triangle_test");
        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "triangle_drawn: OK", result.Output);
        StringAssert.Contains(result.Output, "corner_black: OK",   result.Output);
        StringAssert.Contains(result.Output, "depth_written: OK",  result.Output);

        // Cross-check via framebuffer peripheral: centre pixel (160, 100)
        byte[] px = result.Framebuffer.Pixels;
        int centre = (100 * 320 + 160) * 4;
        Assert.IsTrue(px[centre + 1] > 100,
            $"Centre pixel G={px[centre+1]} — triangle not drawn at (160,100)");
    }

    // ── Test 4: full pipeline renders a block face ───────────────────────────

    [TestMethod]
    public void VoxelRender_BlockFaceIsVisibleFromCamera()
    {
        var result = CompileAndRun("voxel_render_test", maxSteps: 200_000_000);
        Assert.IsTrue(result.Halted, "emulator did not halt");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "render_nonsky: OK", result.Output);

        // Verify via raw framebuffer: at least one pixel near centre is non-sky
        byte[] px = result.Framebuffer.Pixels;
        int cx = 160, cy = 100;
        int skyR_approx = (int)(140 + (float)cy / 200 * 60); // ~170 at y=100
        bool found = false;
        for (int dy = -20; dy <= 20 && !found; dy++)
            for (int dx = -20; dx <= 20 && !found; dx++) {
                int idx = ((cy+dy) * 320 + (cx+dx)) * 4;
                if (idx < 0 || idx + 3 >= px.Length) continue;
                int r = px[idx];
                if (r < skyR_approx - 30) found = true; // grass is darker than sky
            }

        Assert.IsTrue(found, $"No non-sky pixels found near screen centre.\n{result.Output}");
    }

    // ── Test 5: fast fixed-point rasterizer ──────────────────────────────────

    [TestMethod]
    public void VoxelFastRasterizer_DrawsTriangleAndDepthTest()
    {
        var result = CompileAndRun("voxel_fastras_test");
        Assert.IsTrue(result.Halted,    $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "fastras_drawn: OK",  result.Output);
        StringAssert.Contains(result.Output, "fastras_depth: OK",  result.Output);
        StringAssert.Contains(result.Output, "fastras_corner: OK", result.Output);
        StringAssert.Contains(result.Output, "fastras_ztest: OK",  result.Output);

        // Also check via peripheral: centre pixel should be red (R=255, G=0, B=0)
        byte[] px = result.Framebuffer.Pixels;
        int centre = (100 * 320 + 160) * 4;
        Assert.AreEqual(255, (int)px[centre],     $"R channel at centre: {px[centre]}");
        Assert.AreEqual(  0, (int)px[centre + 1], $"G channel at centre: {px[centre+1]}");
        Assert.AreEqual(  0, (int)px[centre + 2], $"B channel at centre: {px[centre+2]}");
    }

    // ── Test 6: world generation ──────────────────────────────────────────────

    [TestMethod]
    public void VoxelWorldGen_PlacesExpectedBlocks()
    {
        var result = CompileAndRun("voxel_worldgen_test", maxSteps: 500_000_000);
        Assert.IsTrue(result.Halted, $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "worldgen_grass: OK",      result.Output);
        StringAssert.Contains(result.Output, "worldgen_dirt_below: OK", result.Output);
        StringAssert.Contains(result.Output, "worldgen_stone: OK",      result.Output);
        StringAssert.Contains(result.Output, "worldgen_not_empty: OK",  result.Output);
    }

    // ── Test 7: player physics ────────────────────────────────────────────────

    [TestMethod]
    public void VoxelPhysics_GravityLandingAndJump()
    {
        var result = CompileAndRun("voxel_physics_test");
        Assert.IsTrue(result.Halted, $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "physics_gravity: OK",     result.Output);
        StringAssert.Contains(result.Output, "physics_landed: OK",      result.Output);
        StringAssert.Contains(result.Output, "physics_land_height: OK", result.Output);
        StringAssert.Contains(result.Output, "physics_on_ground: OK",   result.Output);
        StringAssert.Contains(result.Output, "physics_vel_zero: OK",    result.Output);
        StringAssert.Contains(result.Output, "physics_jump_vel: OK",    result.Output);
        StringAssert.Contains(result.Output, "physics_jump_airborne: OK", result.Output);
    }

    // ── Test 8: MIDI sound effects ────────────────────────────────────────────

    [TestMethod]
    public void VoxelMidi_SoundEffectsDontCrash()
    {
        var result = CompileAndRun("voxel_midi_test");
        Assert.IsTrue(result.Halted, $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "midi_break: OK",    result.Output);
        StringAssert.Contains(result.Output, "midi_place: OK",    result.Output);
        StringAssert.Contains(result.Output, "midi_jump: OK",     result.Output);
        StringAssert.Contains(result.Output, "midi_footstep: OK", result.Output);
        StringAssert.Contains(result.Output, "midi_raw: OK",      result.Output);
    }

    // ── Test 9: full game loop renders a frame ────────────────────────────────

    [TestMethod]
    public void VoxelGameLoop_RendersFrameWithContent()
    {
        var result = CompileAndRun("voxel_gameloop_test", maxSteps: 200_000_000);
        Assert.IsTrue(result.Halted, $"emulator did not halt\nOutput: {result.Output}");
        Assert.AreEqual(0, result.ExitCode);

        StringAssert.Contains(result.Output, "gameloop_rendered: OK",  result.Output);
        StringAssert.Contains(result.Output, "gameloop_crosshair: OK", result.Output);

        // Verify via raw FB: at least 20 non-sky pixels near centre
        byte[] px = result.Framebuffer.Pixels;
        int cx = 160, cy = 100;
        int skyR = (int)(140 + (float)cy / 200 * 60);
        int nonSkyCount = 0;
        for (int dy = -30; dy <= 30; dy++)
            for (int dx = -30; dx <= 30; dx++) {
                int idx = ((cy+dy) * 320 + (cx+dx)) * 4;
                if (idx < 0 || idx + 3 >= px.Length) continue;
                int r = px[idx];
                if (r < skyR - 25 || r > skyR + 25) nonSkyCount++;
            }
        Assert.IsTrue(nonSkyCount >= 20,
            $"Only {nonSkyCount} non-sky pixels near centre — expected a rendered block.\n{result.Output}");
    }
}
