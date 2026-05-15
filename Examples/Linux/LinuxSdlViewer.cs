// LinuxSdlViewer — minimal SDL2 framebuffer display + input bridge for the
// Examples.Linux guest. Runs on its own thread alongside the main CPU loop:
//
//   * Renders FramebufferDevice.PresentedPixels at ~30 fps into an SDL window.
//   * Polls SDL keyboard/mouse events and translates them into Linux KEY_*
//     codes pushed into KeyboardDevice / MouseDevice (whose MMIO regions
//     the guest's rvemu-input daemon polls).
//
// Why not reuse SdlWindow? SdlWindow owns the CPU loop itself and is wired
// to Doom's full peripheral set (DisplayControlDevice, Audio* etc.). The
// Linux example already has its own CPU loop + stdin reader. A focused
// ~150-line viewer is simpler than refactoring SdlWindow.
//
// Lifecycle: viewer.Start() spawns the SDL thread; viewer.Stop() asks it
// to quit and joins.

using System.Diagnostics;
using RiscVEmulator.Core.Peripherals;
using Silk.NET.Maths;
using Silk.NET.SDL;
using Thread = System.Threading.Thread;

namespace Examples.Linux;

public sealed unsafe class LinuxSdlViewer : IDisposable
{
    private readonly FramebufferDevice _fb;
    private readonly KeyboardDevice    _kbd;
    private readonly MouseDevice       _mouse;
    private readonly string            _title;
    private readonly int               _scale;

    private Sdl?     _sdl;
    private Window*  _window;
    private Renderer*_renderer;
    private Texture* _texture;
    private Thread? _thread;
    private volatile bool _running;

    public LinuxSdlViewer(FramebufferDevice fb, KeyboardDevice kbd, MouseDevice mouse,
                          string title = "rvemu Linux", int scale = 3)
    {
        _fb     = fb;
        _kbd    = kbd;
        _mouse  = mouse;
        _title  = title;
        _scale  = scale;
    }

    public void Start()
    {
        _running = true;
        _thread  = new Thread(RunSdlLoop) { IsBackground = true, Name = "Linux-SDL" };
        _thread.Start();
    }

    public void Stop()
    {
        _running = false;
        try { _thread?.Join(500); } catch { }
    }

    public void Dispose() => Stop();

    // ── SDL loop ────────────────────────────────────────────────────────────

    private void RunSdlLoop()
    {
        var sdl = Sdl.GetApi();
        _sdl = sdl;
        if (sdl.Init(Sdl.InitVideo | Sdl.InitEvents) != 0)
        {
            Console.Error.WriteLine($"SDL_Init failed: {sdl.GetErrorS()}");
            return;
        }

        int w = _fb.Width  * _scale;
        int h = _fb.Height * _scale;
        _window = sdl.CreateWindow(_title,
            Sdl.WindowposCentered, Sdl.WindowposCentered, w, h,
            (uint)(WindowFlags.Shown | WindowFlags.Resizable));
        if (_window == null) { Console.Error.WriteLine($"SDL_CreateWindow: {sdl.GetErrorS()}"); return; }

        _renderer = sdl.CreateRenderer(_window, -1,
            (uint)(RendererFlags.Accelerated | RendererFlags.Presentvsync));
        if (_renderer == null) { Console.Error.WriteLine($"SDL_CreateRenderer: {sdl.GetErrorS()}"); return; }

        _texture = sdl.CreateTexture(_renderer,
            (uint)PixelFormatEnum.Abgr8888, (int)TextureAccess.Streaming,
            _fb.Width, _fb.Height);
        if (_texture == null) { Console.Error.WriteLine($"SDL_CreateTexture: {sdl.GetErrorS()}"); return; }

        // Tell SDL the "logical" content size — when the window is
        // resized, SDL handles letterboxing/stretching at render time
        // and remaps mouse coords back into this logical space for us.
        sdl.RenderSetLogicalSize(_renderer, _fb.Width, _fb.Height);

        // Mouse-capture mode — the SDL window grabs the cursor so it
        // can't leave the window, hides the host pointer, and delivers
        // raw deltas with no Windows acceleration. ESC releases the
        // grab; clicking back in the window re-captures.
        bool captured = false;
        void SetCapture(bool on)
        {
            sdl.SetRelativeMouseMode(on ? SdlBool.True : SdlBool.False);
            sdl.ShowCursor(on ? 0 : 1);
            captured = on;
        }
        SetCapture(true);

        // Track which Linux KEY_*s we've reported as DOWN to the guest but
        // haven't yet seen a Keyup for. Used to (a) synthesize a stale
        // Keyup when SDL re-delivers a Keydown for an already-held key
        // (which means we missed the prior Keyup — Windows can drop one
        // on focus change), and (b) flush every held key when the window
        // loses keyboard focus so nothing stays stuck.
        var keysDown = new System.Collections.Generic.HashSet<byte>();
        void ReleaseAllHeldKeys()
        {
            foreach (byte c in keysDown) _kbd.EnqueueKey(c, false);
            keysDown.Clear();
        }

        Event evt;
        var sw = Stopwatch.StartNew();
        double nextFrameMs = 0;

        while (_running)
        {
            // ── Pump events ─────────────────────────────────────────────────
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
                        // Filter SDL's auto-repeat presses. The Linux input
                        // subsystem in the guest generates its own repeats
                        // from a single press + held state.
                        if (evt.Key.Repeat != 0) break;
                        bool pressed = (EventType)evt.Type == EventType.Keydown;
                        int sym = (int)evt.Key.Keysym.Sym;
                        // ESC releases mouse capture. We intercept here and
                        // do NOT forward it to the guest — otherwise the
                        // guest would see a spurious ESC every time the user
                        // wants to un-grab.
                        if (captured && pressed && sym == 0x1b /* SDLK_ESCAPE */)
                        {
                            SetCapture(false);
                            ReleaseAllHeldKeys();
                            break;
                        }
                        // Use the layout-translated Sym (so a key labeled 'P'
                        // on the user's keyboard always maps to Linux KEY_P
                        // regardless of QWERTY/QWERTZ/AZERTY/Dvorak).
                        if (!captured) break;
                        byte code = MapSymToLinuxKey(sym);
                        if (code == 0) break;
                        if (pressed)
                        {
                            // Defend against missing Keyup: if Windows dropped
                            // a previous release for this key (focus glitch),
                            // SDL would re-deliver Keydown without a matching
                            // Keyup, and the guest's kernel input would think
                            // the key never released and auto-repeat. Emit a
                            // synthetic release first, then the fresh press.
                            if (keysDown.Contains(code)) _kbd.EnqueueKey(code, false);
                            _kbd.EnqueueKey(code, true);
                            keysDown.Add(code);
                        }
                        else
                        {
                            _kbd.EnqueueKey(code, false);
                            keysDown.Remove(code);
                        }
                        break;
                    }
                    case EventType.Windowevent:
                    {
                        // SDL2 window event subtypes — focus loss = 12,
                        // keyboard focus loss = 13. Either way, release
                        // every key we believe is held so nothing stays
                        // stuck once the user alt-tabs away.
                        byte sub = evt.Window.Event;
                        if (sub == 12 || sub == 13) ReleaseAllHeldKeys();
                        break;
                    }
                    case EventType.Mousemotion:
                    {
                        if (!captured) break;
                        // In relative mode SDL_Motion delivers raw frame-to-frame
                        // deltas via Xrel/Yrel with zero OS acceleration applied.
                        // Feed them straight through — the guest mouse driver
                        // accumulates and ends up pixel-locked to user intent.
                        int dx = evt.Motion.Xrel;
                        int dy = evt.Motion.Yrel;
                        if (dx != 0 || dy != 0) _mouse.MoveMouse(dx, dy);
                        break;
                    }
                    case EventType.Mousebuttondown:
                    case EventType.Mousebuttonup:
                    {
                        bool pressed = (EventType)evt.Type == EventType.Mousebuttondown;
                        // A click while un-captured re-captures (it's the only
                        // way back in after ESC). We don't forward THIS click
                        // to the guest — it'd land at a stale position.
                        if (!captured)
                        {
                            if (pressed) SetCapture(true);
                            break;
                        }
                        int btn = evt.Button.Button switch
                        {
                            1 => 0, // BTN_LEFT  → bit 0
                            2 => 2, // BTN_MIDDLE → bit 2
                            3 => 1, // BTN_RIGHT → bit 1
                            _ => -1,
                        };
                        if (btn >= 0) _mouse.SetButton(btn, pressed);
                        break;
                    }
                }
            }

            // ── Render at ~30 fps (frame-rate cap) ─────────────────────────
            if (sw.Elapsed.TotalMilliseconds >= nextFrameMs)
            {
                nextFrameMs += 1000.0 / 30.0;
                _fb.PresentFrame();
                fixed (byte* pix = _fb.PresentedPixels)
                {
                    sdl.UpdateTexture(_texture, (Rectangle<int>*)null,
                        pix, _fb.Width * 4);
                }
                sdl.RenderClear(_renderer);
                sdl.RenderCopy(_renderer, _texture, (Rectangle<int>*)null, (Rectangle<int>*)null);
                sdl.RenderPresent(_renderer);
            }
            else
            {
                Thread.Sleep(1);
            }
        }

        if (_texture  != null) sdl.DestroyTexture(_texture);
        if (_renderer != null) sdl.DestroyRenderer(_renderer);
        if (_window   != null) sdl.DestroyWindow(_window);
        sdl.Quit();
    }

    // ── SDL_Keycode (Sym) → Linux KEY_* mapping ────────────────────────────
    //
    // Keysym.Sym is the layout-translated symbol — pressing the German 'Z'
    // key (physical 'Y' position) gives Sym = 'z', not Scancode = SDL_Y.
    // We map those symbols to Linux KEY_* codes so the guest (configured
    // with US layout) types the SAME character the user expected to type.
    //
    // For printable ASCII the Sym value IS the ASCII code (lowercase for
    // letters). Specials (arrows, F-keys, modifiers) use SDL's
    // SDLK_SCANCODE_MASK | scancode (0x40000000 | sc) encoding.

    // SDL packs non-ASCII keys as SDLK_SCANCODE_MASK | scancode = 0x40000000 | sc.
    private const int SDLK_SCANCODE_MASK = 0x40000000;

    private static byte MapSymToLinuxKey(int sym) => sym switch
    {
        // Lower-case letters (Sym == lowercase ASCII)
        'a' => 30, 'b' => 48, 'c' => 46, 'd' => 32, 'e' => 18,
        'f' => 33, 'g' => 34, 'h' => 35, 'i' => 23, 'j' => 36,
        'k' => 37, 'l' => 38, 'm' => 50, 'n' => 49, 'o' => 24,
        'p' => 25, 'q' => 16, 'r' => 19, 's' => 31, 't' => 20,
        'u' => 22, 'v' => 47, 'w' => 17, 'x' => 45, 'y' => 21, 'z' => 44,
        // Upper-case letters — same Linux KEY_*; the guest sees a separate
        // SHIFT-down event for the modifier.
        'A' => 30, 'B' => 48, 'C' => 46, 'D' => 32, 'E' => 18,
        'F' => 33, 'G' => 34, 'H' => 35, 'I' => 23, 'J' => 36,
        'K' => 37, 'L' => 38, 'M' => 50, 'N' => 49, 'O' => 24,
        'P' => 25, 'Q' => 16, 'R' => 19, 'S' => 31, 'T' => 20,
        'U' => 22, 'V' => 47, 'W' => 17, 'X' => 45, 'Y' => 21, 'Z' => 44,
        // Digits 1..9 then 0 → KEY_1..KEY_9 then KEY_0
        '1' =>  2, '2' =>  3, '3' =>  4, '4' =>  5, '5' =>  6,
        '6' =>  7, '7' =>  8, '8' =>  9, '9' => 10, '0' => 11,
        // Punctuation (Sym = ASCII for unshifted form)
        '-'  => 12, '=' => 13, '[' => 26, ']' => 27, '\\' => 43,
        ';'  => 39, '\''=> 40, '`' => 41, ','=> 51, '.'  => 52, '/' => 53,
        // Specials with ASCII-range Sym
        '\b' => 14, '\t' => 15, '\r' => 28, ' ' => 57, 0x1b => 1,
        // F-keys F1..F12 (SDL_SCANCODE_MASK | 58..69)
        0x4000003A => 59,  0x4000003B => 60, 0x4000003C => 61, 0x4000003D => 62,
        0x4000003E => 63,  0x4000003F => 64, 0x40000040 => 65, 0x40000041 => 66,
        0x40000042 => 67,  0x40000043 => 68, 0x40000044 => 87, 0x40000045 => 88,
        // Arrows (SC 79=RIGHT 80=LEFT 81=DOWN 82=UP)
        0x4000004F => 106, 0x40000050 => 105, 0x40000051 => 108, 0x40000052 => 103,
        // Nav cluster (DELETE INSERT HOME END PAGEUP PAGEDOWN)
        0x4000004C => 111, 0x40000049 => 110, 0x4000004A => 102,
        0x4000004D => 107, 0x4000004B => 104, 0x4000004E => 109,
        // Modifiers (LCTRL LSHIFT LALT LGUI RCTRL RSHIFT RALT RGUI)
        0x400000E0 => 29,  0x400000E1 => 42,  0x400000E2 => 56,  0x400000E3 => 125,
        0x400000E4 => 97,  0x400000E5 => 54,  0x400000E6 => 100, 0x400000E7 => 126,
        // CapsLock (SC 57)
        0x40000039 => 58,
        _ => 0,
    };
}
