using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Tests for the Voxel renderer building blocks. Each test isolates one
/// layer of the engine:
///   1. MkRgba   — pixel packing byte order
///   2. WorldGen — terrain block placement
///   3. Physics  — gravity, landing, jump
///   4. Midi     — sound-effect MIDI emission
/// All tests compile with -march=rv32i (software multiply and divide).
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
    /// Compile one or more sources to an ELF using rv32i (multiply and
    /// division resolved to runtime.c software libcalls).
    /// Adds -I{VoxelProgramsDir} and -I{RuntimeDir} so test C files can
    /// <c>#include "voxel_main.c"</c>.
    /// </summary>
    private static void CompileMExt(IEnumerable<string> srcFiles, string elfFile)
    {
        var args = new List<string>
        {
            "--target=riscv32-unknown-elf",
            "-march=rv32i", "-mabi=ilp32",
            "-nostdlib", "-nostartfiles", "-O3",
            "-fno-builtin", "-fsigned-char",
            "-fuse-ld=lld",
            $"-I{VoxelProgramsDir}",
            $"-I{RuntimeDir}",
            $"-Wl,-T,{LinkerScript}",
        };
        args.AddRange(srcFiles);
        args.Add("-o");
        args.Add(elfFile);

        int exit = WslClang.Run(args, out string stderr);
        Assert.IsTrue(exit == 0, $"clang failed (exit {exit}).\nstderr: {stderr}");
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
        return RunElfFull(elfFile, maxSteps);
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

    // ── Test 2: world generation ─────────────────────────────────────────────

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

    // ── Test 3: player physics ───────────────────────────────────────────────

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

    // ── Test 4: MIDI sound effects ───────────────────────────────────────────

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
}
