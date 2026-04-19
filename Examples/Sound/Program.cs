using System.Diagnostics;
using Silk.NET.SDL;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

// Resolve shared runtime files
string solutionRoot = Path.GetFullPath(Path.Combine(exeDir, "..", "..", "..", "..", ".."));
string runtimeDir   = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Runtime");
string runtimeC     = Path.Combine(runtimeDir, "runtime.c");
string libcC        = Path.Combine(runtimeDir, "libc.c");
string softfloatC   = Path.Combine(runtimeDir, "softfloat.c");
string syscallsC    = Path.Combine(runtimeDir, "syscalls.c");
string linkerLd     = Path.Combine(solutionRoot, "RiscVEmulator.Tests", "Programs", "linker.ld");
string demoC        = Path.Combine(exeDir, "Programs", "sound_demo.c");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);

// ── Compile ──────────────────────────────────────────────────────
Console.WriteLine("Compiling sound demo...");

string runtimeObj   = Path.Combine(buildDir, "runtime.o");
string libcObj      = Path.Combine(buildDir, "libc.o");
string softfloatObj = Path.Combine(buildDir, "softfloat.o");
string syscallsObj  = Path.Combine(buildDir, "syscalls.o");
string demoObj      = Path.Combine(buildDir, "sound_demo.o");
string elfPath      = Path.Combine(buildDir, "sound_demo.elf");

CompileObj(runtimeC, runtimeObj);
CompileObj(libcC, libcObj);
CompileObj(softfloatC, softfloatObj);
CompileObj(syscallsC, syscallsObj);
CompileObj(demoC, demoObj);
Link(new[] { demoObj, libcObj, softfloatObj, syscallsObj, runtimeObj }, elfPath);

Console.WriteLine($"  sound_demo.elf: {new FileInfo(elfPath).Length:N0} bytes");

// ── Build SoC (no framebuffer needed, but register all for bus) ──
var memory  = new Memory(4 * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
var fb      = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
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

uart.OutputHandler = c => Console.Write(c);

byte[] elfData = File.ReadAllBytes(elfPath);
var regs = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, 0x003FFF00u);

var emu = new Emulator(bus, regs, entry);
emu.OutputHandler = c => Console.Write(c);

// ── Run with audio-only SDL (no window for rendering) ────────────
// We use a small SDL window to capture keyboard input for pitch control
Console.WriteLine("Starting sound demo (W/S = freq, A/D = volume)...");

unsafe
{
    var sdl = Sdl.GetApi();
    if (sdl.Init(Sdl.InitAudio | Sdl.InitVideo | Sdl.InitEvents) < 0)
    {
        Console.Error.WriteLine($"SDL_Init failed: {sdl.GetErrorS()}");
        return 1;
    }

    // Small control window for keyboard input
    var window = sdl.CreateWindow("Sound Demo — RV32I (keys: WASD)",
        Sdl.WindowposUndefined, Sdl.WindowposUndefined,
        400, 100, (uint)WindowFlags.Shown);

    // Audio device — lazy open when guest starts playing
    uint audioDevice = 0;
    uint lastRate = 0;
    byte lastChan = 0;

    // CPU thread
    bool running = true;
    var cpuThread = new System.Threading.Thread(() =>
    {
        while (running && !emu.IsHalted)
        {
            for (int i = 0; i < 500_000 && running && !emu.IsHalted; i++)
                emu.Step();
        }
    })
    { IsBackground = true, Name = "RV32I-CPU" };
    cpuThread.Start();

    // SDL event loop
    Event evt;
    while (running && !emu.IsHalted)
    {
        while (sdl.PollEvent(&evt) != 0)
        {
            switch ((EventType)evt.Type)
            {
                case EventType.Quit:
                    running = false;
                    break;
                case EventType.Keydown:
                case EventType.Keyup:
                {
                    bool pressed = (EventType)evt.Type == EventType.Keydown;
                    uint sym = (uint)evt.Key.Keysym.Sym;
                    byte code = sym < 128 ? (byte)sym : (byte)(sym switch
                    {
                        0x40000052 => 0x26, // UP
                        0x40000051 => 0x28, // DOWN
                        0x4000004F => 0x27, // RIGHT
                        0x40000050 => 0x25, // LEFT
                        _ => 0
                    });
                    if (code != 0) kbd.EnqueueKey(code, pressed);
                    break;
                }
            }
        }

        // Audio drain — always queue when guest signals, never drop
        if (audioCtrl.IsPlaying && audioCtrl.BufLength > 0)
        {
            uint rate = audioCtrl.SampleRate;
            byte chan = (byte)audioCtrl.Channels;
            if (rate != lastRate || chan != lastChan)
            {
                if (audioDevice > 0) sdl.CloseAudioDevice(audioDevice);
                AudioSpec want = new()
                {
                    Freq = (int)rate,
                    Format = Sdl.AudioS16Lsb,
                    Channels = chan,
                    Samples = 2048,
                };
                AudioSpec have;
                audioDevice = sdl.OpenAudioDevice((byte*)null, 0, &want, &have, 0);
                if (audioDevice > 0) sdl.PauseAudioDevice(audioDevice, 0);
                lastRate = rate;
                lastChan = chan;
            }

            if (audioDevice > 0)
            {
                uint start = audioCtrl.BufStart;
                uint len = Math.Min(audioCtrl.BufLength,
                                    (uint)audioBuf.Buffer.Length - start);
                // Only queue when SDL has consumed enough ahead — keeps at most 2 buffers
                // queued. This throttles the emulator to real-time audio speed, eliminating
                // the stale-audio lag and the apparent 15-second input delay.
                uint queued = sdl.GetQueuedAudioSize(audioDevice);
                if (len > 0 && queued < 2 * len)
                {
                    fixed (byte* buf = &audioBuf.Buffer[start])
                        sdl.QueueAudio(audioDevice, buf, len);
                    // Clear play bit so guest can submit the next buffer
                    audioCtrl.Ctrl = 0;
                }
            }
        }

        System.Threading.Thread.Sleep(1); // fast polling for double-buffer handshake
    }

    running = false;
    cpuThread.Join(2000);
    if (audioDevice > 0) sdl.CloseAudioDevice(audioDevice);
    if (window != null) sdl.DestroyWindow(window);
    sdl.Quit();
}

return 0;

// ── Helpers ──────────────────────────────────────────────────────
void CompileObj(string src, string obj)
{
    Console.Write($"  {Path.GetFileName(src)}... ");
    RunClang(new[] {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char", "-c",
        $"-I{runtimeDir}", src, "-o", obj
    });
    Console.WriteLine("OK");
}

void Link(string[] objs, string elf)
{
    Console.Write("  Linking... ");
    var linkArgs = new List<string> {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objs);
    linkArgs.AddRange(new[] { "-o", elf });
    RunClang(linkArgs.ToArray());
    Console.WriteLine("OK");
}

void RunClang(string[] clangArgs)
{
    var psi = new ProcessStartInfo(clang)
    {
        RedirectStandardError = true,
        RedirectStandardOutput = true,
        UseShellExecute = false,
    };
    foreach (var a in clangArgs)
        psi.ArgumentList.Add(a);
    var proc = Process.Start(psi)!;
    string stderr = proc.StandardError.ReadToEnd();
    proc.WaitForExit();
    if (proc.ExitCode != 0)
    {
        Console.Error.WriteLine($"FAILED");
        Console.Error.WriteLine(stderr);
        Environment.Exit(1);
    }
}
