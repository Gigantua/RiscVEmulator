using System.Diagnostics;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Tests;

/// <summary>
/// Shared helpers for all emulator integration tests.
/// All programs compile for pure RV32I (-march=rv32i) and link against
/// Runtime/runtime.c (software mul/div).  Float/double tests additionally
/// link Runtime/softfloat.c.
/// </summary>
public abstract class EmulatorTestBase
{
    protected static readonly string TestDir = AppContext.BaseDirectory;
    protected static readonly string ProgramDir = Path.Combine(TestDir, "Programs");
    protected static readonly string RuntimeDir = Path.Combine(TestDir, "Runtime");
    protected static readonly string LinkerScript = Path.Combine(RuntimeDir, "linker.ld");

    /// <summary>Compile one or more C/object files to a RISC-V ELF using clang.</summary>
    protected static void CompileC(IEnumerable<string> srcFiles, string elfFile)
    {
        Assert.IsTrue(File.Exists(LinkerScript), $"linker.ld not found at {LinkerScript}");

        var psi = new ProcessStartInfo("clang")
        {
            WorkingDirectory       = TestDir,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            UseShellExecute        = false,
        };
        psi.ArgumentList.Add("--target=riscv32-unknown-elf");
        psi.ArgumentList.Add("-march=rv32i");
        psi.ArgumentList.Add("-mabi=ilp32");
        psi.ArgumentList.Add("-nostdlib");
        psi.ArgumentList.Add("-nostartfiles");
        psi.ArgumentList.Add("-O3");
        psi.ArgumentList.Add("-fno-builtin");
        psi.ArgumentList.Add("-fuse-ld=lld");
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

    /// <summary>Compile a single C source to an object file.</summary>
    protected static void CompileToObject(string srcFile, string objFile, string optLevel)
    {
        var psi = new ProcessStartInfo("clang")
        {
            WorkingDirectory       = TestDir,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            UseShellExecute        = false,
        };
        psi.ArgumentList.Add("--target=riscv32-unknown-elf");
        psi.ArgumentList.Add("-march=rv32i");
        psi.ArgumentList.Add("-mabi=ilp32");
        psi.ArgumentList.Add(optLevel);
        psi.ArgumentList.Add("-fno-builtin");
        psi.ArgumentList.Add($"-I{RuntimeDir}");
        psi.ArgumentList.Add("-c");
        psi.ArgumentList.Add(srcFile);
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add(objFile);

        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to start clang.");
        string stdout = proc.StandardOutput.ReadToEnd();
        string stderr = proc.StandardError.ReadToEnd();
        proc.WaitForExit();

        Assert.IsTrue(proc.ExitCode == 0,
            $"clang -c failed (exit {proc.ExitCode}).\nstdout: {stdout}\nstderr: {stderr}");
    }

    /// <summary>Build runtime.o from Runtime/runtime.c (cached per test run).</summary>
    protected static string EnsureRuntimeObject()
    {
        string rtSrc = Path.Combine(RuntimeDir, "runtime.c");
        string rtObj = Path.Combine(TestDir, "runtime.o");
        Assert.IsTrue(File.Exists(rtSrc), $"runtime.c not found at {rtSrc}");
        CompileToObject(rtSrc, rtObj, "-O3");
        return rtObj;
    }

    /// <summary>Build malloc.o from Runtime/malloc.c.</summary>
    protected static string EnsureMallocObject()
    {
        string src = Path.Combine(RuntimeDir, "malloc.c");
        string obj = Path.Combine(TestDir, "malloc_rt.o");
        Assert.IsTrue(File.Exists(src), $"malloc.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    /// <summary>Build vfs.o from Runtime/vfs.c.</summary>
    protected static string EnsureVfsObject()
    {
        string src = Path.Combine(RuntimeDir, "vfs.c");
        string obj = Path.Combine(TestDir, "vfs_rt.o");
        Assert.IsTrue(File.Exists(src), $"vfs.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    /// <summary>Build softfloat.o from Runtime/softfloat.c.</summary>
    protected static string EnsureSoftFloatObject()
    {
        string src = Path.Combine(RuntimeDir, "softfloat.c");
        string obj = Path.Combine(TestDir, "softfloat.o");
        Assert.IsTrue(File.Exists(src), $"softfloat.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    /// <summary>Build syscalls.o from Runtime/syscalls.c.</summary>
    protected static string EnsureSyscallsObject()
    {
        string src = Path.Combine(RuntimeDir, "syscalls.c");
        string obj = Path.Combine(TestDir, "syscalls.o");
        Assert.IsTrue(File.Exists(src), $"syscalls.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    /// <summary>Build libc.o from Runtime/libc.c.</summary>
    protected static string EnsureLibcObject()
    {
        string src = Path.Combine(RuntimeDir, "libc.c");
        string obj = Path.Combine(TestDir, "libc.o");
        Assert.IsTrue(File.Exists(src), $"libc.c not found at {src}");
        CompileToObject(src, obj, "-O3");
        return obj;
    }

    /// <summary>Load and run an ELF file. Returns (output, exitCode, isHalted).</summary>
    protected static (string Output, int ExitCode, bool Halted) RunElf(
        string elfFile, int maxSteps = 20_000_000)
    {
        var result = RunElfFull(elfFile, maxSteps);
        return (result.Output, result.ExitCode, result.Halted);
    }

    /// <summary>Run result with access to peripherals for post-run inspection.</summary>
    protected record RunResult(
        string Output, int ExitCode, bool Halted,
        MemoryBus Bus, FramebufferDevice Framebuffer, DisplayControlDevice DisplayControl,
        KeyboardDevice Keyboard, MouseDevice Mouse,
        AudioBufferDevice AudioBuffer, AudioControlDevice AudioControl,
        RealTimeClockDevice RealTimeClock = null!);

    /// <summary>Load and run an ELF file, returning full peripheral access.</summary>
    protected static RunResult RunElfFull(string elfFile, int maxSteps = 20_000_000,
        Action<KeyboardDevice>? setupKeyboard = null,
        Action<MouseDevice>? setupMouse = null,
        Action<UartDevice>? setupUart = null,
        bool enableMExtension = false)
    {
        byte[] elfData = File.ReadAllBytes(elfFile);
        var memory  = new Memory(16 * 1024 * 1024);
        var bus     = new MemoryBus(memory);
        var uart    = new UartDevice();
        var fb      = new FramebufferDevice();
        var display = new DisplayControlDevice(fb);
        display.SetMemory(memory);
        var kbd     = new KeyboardDevice();
        var mouse   = new MouseDevice();
        var rtc     = new RealTimeClockDevice();
        var audioBuf  = new AudioBufferDevice();
        var audioCtrl = new AudioControlDevice();
        bus.RegisterPeripheral(uart);
        bus.RegisterPeripheral(fb);
        bus.RegisterPeripheral(display);
        bus.RegisterPeripheral(kbd);
        bus.RegisterPeripheral(mouse);
        bus.RegisterPeripheral(rtc);
        bus.RegisterPeripheral(audioBuf);
        bus.RegisterPeripheral(audioCtrl);

        // Let test inject keyboard/mouse/uart events before run
        setupKeyboard?.Invoke(kbd);
        setupMouse?.Invoke(mouse);
        setupUart?.Invoke(uart);

        var regs   = new RegisterFile();
        uint entry = ElfLoader.Load(elfData, bus);
        regs.Write(2, 0x00800000u); // sp = 8 MB

        var sb  = new StringBuilder();
        uart.OutputHandler = c => sb.Append(c);

        var emu = new Emulator(bus, regs, entry);
        emu.OutputHandler = c => sb.Append(c);
        if (enableMExtension) emu.EnableMExtension = true;
        emu.Run(maxSteps);

        return new RunResult(sb.ToString(), emu.ExitCode, emu.IsHalted, bus, fb, display, kbd, mouse, audioBuf, audioCtrl, rtc);
    }

    /// <summary>
    /// Compile a C program with libc + softfloat + syscalls + runtime and run it.
    /// Provides printf, puts, putchar, snprintf, string, stdlib, math functions.
    /// Programs use _start as entry and can call printf("%d %f", ...) etc.
    /// </summary>
    protected static string RunProgramWithLibc(string baseName, int maxSteps = 20_000_000)
    {
        string srcFile = Path.Combine(ProgramDir, $"{baseName}.c");
        string elfFile = Path.Combine(TestDir, $"{baseName}.elf");

        Assert.IsTrue(File.Exists(srcFile), $"{baseName}.c not found at {srcFile}");

        string rtObj   = EnsureRuntimeObject();
        string sfObj   = EnsureSoftFloatObject();
        string scObj   = EnsureSyscallsObject();
        string lcObj   = EnsureLibcObject();
        CompileC(new[] { srcFile, lcObj, sfObj, scObj, rtObj }, elfFile);

        var (output, exitCode, halted) = RunElf(elfFile, maxSteps);
        Assert.IsTrue(halted,      $"{baseName}: emulator did not halt within {maxSteps} steps.");
        Assert.AreEqual(0, exitCode, $"{baseName}: exit code was {exitCode}, expected 0.");
        return output;
    }

}
