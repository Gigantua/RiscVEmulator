using System.Diagnostics;
using System.Runtime.InteropServices;
using Silk.NET.SDL;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Frontend;

/// <summary>
/// Configuration options for <see cref="SdlWindow"/>.
/// </summary>
public class SdlWindowOptions
{
    public int Scale { get; set; } = 3;
    public int TargetFps { get; set; } = 120;
    public string Title { get; set; } = "RV32I Emulator";
    public bool GrabMouse { get; set; } = true;
}

/// <summary>
/// SDL2 frontend — fully decoupled from the CPU.
///
/// Architecture:
///   CPU thread  — runs the emulator at full speed, interacts only via MMIO
///   Main thread — SDL event loop + render loop at target FPS
///
/// All communication is through memory-mapped peripherals:
///   Framebuffer (0x20000000)   — CPU writes pixels, SDL reads them to render
///   Display Ctrl (0x20100000)  — CPU can set VSYNC flag to request present
///   Keyboard (0x10001000)      — SDL enqueues key events, CPU polls STATUS/DATA
///   Mouse (0x10002000)         — SDL writes deltas/buttons, CPU reads them
///   Audio (0x30000000+)        — CPU fills buffer + sets control, SDL drains to speakers
///
/// The render loop always displays whatever is in the framebuffer — no synchronization
/// required. Guest code can optionally write VSYNC_FLAG=1 to hint a frame is complete,
/// but the display updates regardless.
/// </summary>
public unsafe class SdlWindow
{
    private readonly FramebufferDevice _fb;
    private readonly DisplayControlDevice _display;
    private readonly KeyboardDevice _kbd;
    private readonly MouseDevice _mouse;
    private readonly AudioBufferDevice _audioBuf;
    private readonly AudioControlDevice _audioCtrl;
    private readonly MidiDevice? _midi;
    private readonly Emulator _emu;
    private readonly SdlWindowOptions _opts;

    private volatile bool _running = true;
    private long _totalSteps;

    // Audio state — shared between render loop and audio-drain thread
    private Sdl? _sdlApi;
    private uint _audioDevice;
    private uint _lastAudioRate;
    private byte _lastAudioChan;
    private uint _lastQueuedGen = unchecked((uint)-1);
    private readonly object _audioLock = new();

    public SdlWindow(
        FramebufferDevice fb, DisplayControlDevice display,
        KeyboardDevice kbd, MouseDevice mouse,
        AudioBufferDevice audioBuf, AudioControlDevice audioCtrl,
        Emulator emu, SdlWindowOptions? options = null,
        MidiDevice? midi = null)
    {
        _fb = fb;
        _display = display;
        _kbd = kbd;
        _mouse = mouse;
        _audioBuf = audioBuf;
        _audioCtrl = audioCtrl;
        _midi = midi;
        _emu = emu;
        _opts = options ?? new SdlWindowOptions();
    }

    // ── SDL Keysym → guest keycode mapping ──────────────────────────

    private static byte MapKeysym(uint sym)
    {
        // Printable ASCII passes through directly
        if (sym < 128) return (byte)sym;

        // Non-printable SDL keys (0x40000000+) → codes doom_main.c expects
        return sym switch
        {
            0x4000004F => 0x27, // RIGHT
            0x40000050 => 0x25, // LEFT
            0x40000051 => 0x28, // DOWN
            0x40000052 => 0x26, // UP
            0x400000E0 => 0x11, // LCTRL
            0x400000E1 => 0x10, // LSHIFT
            0x400000E2 => 0x12, // LALT
            0x400000E4 => 0x11, // RCTRL
            0x400000E5 => 0x10, // RSHIFT
            0x400000E6 => 0x12, // RALT
            0x40000039 => 0x14, // CAPSLOCK
            0x40000049 => 0x2D, // INSERT
            0x4000004A => 0x24, // HOME
            0x4000004B => 0x21, // PAGEUP
            0x4000004D => 0x23, // END
            0x4000004E => 0x22, // PAGEDOWN
            _ => 0
        };
    }

    // ── CPU thread ──────────────────────────────────────────────────

    private void CpuThread()
    {
        while (_running && !_emu.IsHalted)
        {
            // Single P/Invoke call runs entire batch in native C++
            int executed = _emu.StepN(2_000_000);
            Interlocked.Add(ref _totalSteps, executed);
        }
    }

    // ── Audio drain (called from both render loop and audio thread) ─

    private unsafe void DrainAudioQueue()
    {
        var sdl = _sdlApi;
        if (sdl == null || _audioDevice == 0) return;
        if (!_audioCtrl.IsPlaying || _audioCtrl.BufLength == 0) return;

        uint gen    = _audioCtrl.WriteGeneration;
        bool newData = gen != _lastQueuedGen;

        // Only queue when the guest has signalled new data (WriteGeneration changed).
        // Reading BufStart/BufLength is only safe AFTER the guest writes CTRL=1
        // (which increments WriteGeneration), guaranteeing all registers are set.
        if (newData)
        {
            uint queued = sdl.GetQueuedAudioSize(_audioDevice);
            if (queued < 8192)
            {
                _lastQueuedGen = gen;
                uint start = _audioCtrl.BufStart;
                uint len = Math.Min(_audioCtrl.BufLength,
                                    (uint)_audioBuf.Buffer.Length - start);
                if (len > 0)
                {
                    fixed (byte* buf = &_audioBuf.Buffer[start])
                        sdl.QueueAudio(_audioDevice, buf, len);
                }
                _audioCtrl.Ctrl = 0;
            }
        }
    }

    private void AudioDrainThread()
    {
        while (_running)
        {
            System.Threading.Thread.Sleep(8);
            lock (_audioLock) { DrainAudioQueue(); }
        }
    }

    // ── Main entry point (SDL main thread) ──────────────────────────

    public int Run()
    {
        var sdl = Sdl.GetApi();
        _sdlApi = sdl;

        if (sdl.Init(Sdl.InitVideo | Sdl.InitAudio | Sdl.InitEvents) < 0)
        {
            Console.Error.WriteLine($"SDL_Init failed: {sdl.GetErrorS()}");
            return 1;
        }

        int fbW = _fb.Width;
        int fbH = _fb.Height;

        var window = sdl.CreateWindow(
            _opts.Title,
            Sdl.WindowposUndefined, Sdl.WindowposUndefined,
            fbW * _opts.Scale, fbH * _opts.Scale, (uint)WindowFlags.Shown);

        if (window == null)
        {
            Console.Error.WriteLine($"SDL_CreateWindow failed: {sdl.GetErrorS()}");
            sdl.Quit();
            return 1;
        }

        // Mouse grab (configurable — Doom wants it, demos typically don't)
        bool mouseGrabbed = _opts.GrabMouse;
        if (mouseGrabbed)
        {
            sdl.SetRelativeMouseMode(SdlBool.True);
            sdl.SetWindowGrab(window, SdlBool.True);
        }

        // No vsync — we control timing ourselves
        var renderer = sdl.CreateRenderer(window, -1, (uint)RendererFlags.Accelerated);
        if (renderer == null)
        {
            Console.Error.WriteLine($"SDL_CreateRenderer failed: {sdl.GetErrorS()}");
            sdl.DestroyWindow(window);
            sdl.Quit();
            return 1;
        }

        var texture = sdl.CreateTexture(renderer,
            (uint)PixelFormatEnum.Abgr8888,
            (int)TextureAccess.Streaming,
            fbW, fbH);

        if (texture == null)
        {
            Console.Error.WriteLine($"SDL_CreateTexture failed: {sdl.GetErrorS()}");
            sdl.DestroyRenderer(renderer);
            sdl.DestroyWindow(window);
            sdl.Quit();
            return 1;
        }

        // ── Audio: lazy open when guest first writes config ────────
        // Audio state is in class fields — shared with AudioDrainThread

        // ── Start CPU thread ──────────────────────────────────────
        var cpuThread = new System.Threading.Thread(CpuThread)
        {
            IsBackground = true,
            Name = "RV32I-CPU",
            Priority = System.Threading.ThreadPriority.AboveNormal
        };
        cpuThread.Start();

        // ── Audio drain thread (survives window drag stalls) ──────
        var audioThread = new System.Threading.Thread(AudioDrainThread)
        {
            IsBackground = true,
            Name = "Audio-Drain"
        };
        audioThread.Start();

        // ── Stats ─────────────────────────────────────────────────
        var wallClock = Stopwatch.StartNew();
        long lastStatSteps = 0;
        double lastStatTime = 0;

        // ── Render loop (main thread) ─────────────────────────────
        double frameMs = 1000.0 / _opts.TargetFps;
        Event evt;
        var frameSw = new Stopwatch();

        while (_running && !_emu.IsHalted)
        {
            frameSw.Restart();

            // 1. Pump SDL events → write to MMIO peripherals
            while (sdl.PollEvent(&evt) != 0)
            {
                switch ((EventType)evt.Type)
                {
                    case EventType.Quit:
                        _running = false;
                        break;

                    case EventType.Keydown:
                    case EventType.Keyup:
                    {
                        // Filter auto-repeat events (only initial press/release)
                        if (evt.Key.Repeat != 0) break;

                        bool pressed = (EventType)evt.Type == EventType.Keydown;
                        uint sym = (uint)evt.Key.Keysym.Sym;

                        // F10 toggles mouse grab
                        if (sym == 0x40000043 && pressed) // SDLK_F10
                        {
                            mouseGrabbed = !mouseGrabbed;
                            sdl.SetRelativeMouseMode(mouseGrabbed ? SdlBool.True : SdlBool.False);
                            sdl.SetWindowGrab(window, mouseGrabbed ? SdlBool.True : SdlBool.False);
                            break;
                        }

                        byte code = MapKeysym(sym);
                        if (code != 0)
                            _kbd.EnqueueKey(code, pressed);
                        var mod = evt.Key.Keysym.Mod;
                        _kbd.SetModifiers(
                            (mod & (ushort)Keymod.Shift) != 0,
                            (mod & (ushort)Keymod.Ctrl) != 0,
                            (mod & (ushort)Keymod.Alt) != 0);
                        break;
                    }

                    case EventType.Mousemotion:
                        _mouse.MoveMouse(evt.Motion.Xrel, evt.Motion.Yrel);
                        break;

                    case EventType.Mousebuttondown:
                    case EventType.Mousebuttonup:
                    {
                        bool pressed = (EventType)evt.Type == EventType.Mousebuttondown;
                        int button = evt.Button.Button switch
                        {
                            1 => 0, 2 => 2, 3 => 1, _ => -1
                        };
                        if (button >= 0) _mouse.SetButton(button, pressed);
                        break;
                    }
                }
            }

            // 2. Blit framebuffer → SDL texture.
            //    Guests that signal vsync (Voxel): PresentFrame() was called on vsync write,
            //    so PresentedPixels holds a complete frame — no tearing, no mid-frame flicker.
            //    Guests that never signal vsync (Doom): auto-present every SDL frame
            //    so we show live pixels exactly as before.
            if (!_display.VsyncEverUsed)
                _fb.PresentFrame();

            byte* pixels;
            int pitch;
            if (sdl.LockTexture(texture, null, (void**)&pixels, &pitch) == 0)
            {
                fixed (byte* fbPtr = _fb.PresentedPixels)
                {
                    int rowBytes = fbW * 4;
                    for (int y = 0; y < fbH; y++)
                    {
                        Buffer.MemoryCopy(
                            fbPtr + y * rowBytes,
                            pixels + y * pitch,
                            pitch, rowBytes);
                    }
                }
                sdl.UnlockTexture(texture);
            }

            sdl.RenderClear(renderer);
            sdl.RenderCopy(renderer, texture, null, null);
            sdl.RenderPresent(renderer);

            // If guest set VSYNC flag, acknowledge it
            if (_display.VsyncFlag != 0)
                _display.ClearVsync();

            // 3. Audio: open/reopen device if guest changed config, drain buffer
            lock (_audioLock)
            {
                if (_audioCtrl.IsPlaying && _audioCtrl.BufLength > 0)
                {
                    uint rate = _audioCtrl.SampleRate;
                    byte chan = (byte)_audioCtrl.Channels;
                    if (rate != _lastAudioRate || chan != _lastAudioChan)
                    {
                        if (_audioDevice > 0) sdl.CloseAudioDevice(_audioDevice);
                        AudioSpec want = new()
                        {
                            Freq = (int)rate,
                            Format = Sdl.AudioS16Lsb,
                            Channels = chan,
                            Samples = 512,
                        };
                        AudioSpec have;
                        _audioDevice = sdl.OpenAudioDevice((byte*)null, 0, &want, &have, 0);
                        if (_audioDevice > 0)
                            sdl.PauseAudioDevice(_audioDevice, 0);
                        _lastAudioRate = rate;
                        _lastAudioChan = chan;
                    }
                }

                DrainAudioQueue();
            }

            // 4. Stats (every 2 seconds)
            double elapsed = wallClock.Elapsed.TotalSeconds;
            if (elapsed - lastStatTime >= 2.0)
            {
                long steps = Interlocked.Read(ref _totalSteps);
                double mips = (steps - lastStatSteps) / ((elapsed - lastStatTime) * 1_000_000);
                Console.Write($"\r[{elapsed:F0}s] {steps / 1_000_000}M steps, {mips:F1} MIPS   ");
                lastStatSteps = steps;
                lastStatTime = elapsed;
            }

            // 5. Frame rate limiter
            double frameElapsed = frameSw.Elapsed.TotalMilliseconds;
            if (frameElapsed < frameMs)
                System.Threading.Thread.Sleep(Math.Max(1, (int)(frameMs - frameElapsed)));
        }

        // ── Shutdown ──────────────────────────────────────────────
        _running = false;
        cpuThread.Join(2000);
        audioThread.Join(500);

        if (_audioDevice > 0) sdl.CloseAudioDevice(_audioDevice);
        _midi?.Dispose();
        sdl.DestroyTexture(texture);
        sdl.DestroyRenderer(renderer);
        sdl.DestroyWindow(window);
        sdl.Quit();

        long total = Interlocked.Read(ref _totalSteps);
        double totalSec = wallClock.Elapsed.TotalSeconds;
        Console.WriteLine($"\nExited (code {_emu.ExitCode}), {total:N0} steps in {totalSec:F1}s ({total / totalSec / 1_000_000:F1} MIPS avg)");
        return _emu.ExitCode;
    }
}
