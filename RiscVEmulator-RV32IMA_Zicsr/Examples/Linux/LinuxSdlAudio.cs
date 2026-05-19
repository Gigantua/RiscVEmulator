// LinuxSdlAudio — host-side SDL2 audio drain for the Examples.Linux guest.
//
// Mirrors what SdlWindow does for the bare-metal demos: opens an SDL audio
// device on demand (when the guest writes a non-zero sample rate / channel
// count), then drains the AudioBufferDevice into SDL's queue whenever the
// guest bumps the control register.
//
// Hand-off protocol (same as Doom uses, see Core/Peripherals/AudioDevice.cs):
//   1. Guest fills PCM samples into the buffer at 0x30000000.
//   2. Guest writes SampleRate / Channels / BufStart / BufLength to the
//      control device at 0x30100000.
//   3. Guest sets Ctrl bit 0 (= "play"). AudioControlDevice.Write increments
//      WriteGeneration as the publish fence.
//   4. We notice WriteGeneration changed, SDL_QueueAudio the slice, and
//      clear Ctrl so the guest knows the slot is free.
//
// Runs on its own thread alongside LinuxSdlViewer. SDL_Init is idempotent
// across subsystems, so initialising InitAudio from this thread when the
// viewer thread has already initialised InitVideo|InitEvents is safe.

using RiscVEmulator.Core.Peripherals;
using Silk.NET.SDL;
using Thread = System.Threading.Thread;

namespace Examples.Linux;

public sealed unsafe class LinuxSdlAudio : IDisposable
{
    private readonly AudioBufferDevice  _buf;
    private readonly AudioControlDevice _ctrl;

    private Sdl?    _sdl;
    private uint    _device;
    private uint    _lastRate;
    private byte    _lastChan;
    private uint    _lastQueuedGen = unchecked((uint)-1);

    private Thread?       _thread;
    private volatile bool _running;

    public LinuxSdlAudio(AudioBufferDevice buf, AudioControlDevice ctrl)
    {
        _buf  = buf;
        _ctrl = ctrl;
    }

    public void Start()
    {
        _running = true;
        _thread  = new Thread(Run) { IsBackground = true, Name = "Linux-SDL-Audio" };
        _thread.Start();
    }

    public void Stop()
    {
        _running = false;
        try { _thread?.Join(500); } catch { }
    }

    public void Dispose() => Stop();

    private void Run()
    {
        var sdl = Sdl.GetApi();
        _sdl = sdl;

        if (sdl.Init(Sdl.InitAudio) < 0)
        {
            Console.Error.WriteLine($"SDL_Init(audio) failed: {sdl.GetErrorS()}");
            return;
        }

        while (_running)
        {
            EnsureDeviceOpen();
            Drain();
            Thread.Sleep(8);
        }

        if (_device > 0) sdl.CloseAudioDevice(_device);
    }

    private void EnsureDeviceOpen()
    {
        var sdl = _sdl;
        if (sdl == null) return;
        if (!_ctrl.IsPlaying || _ctrl.BufLength == 0) return;

        uint rate = _ctrl.SampleRate;
        byte chan = (byte)_ctrl.Channels;
        if (rate == _lastRate && chan == _lastChan && _device > 0) return;

        if (_device > 0) sdl.CloseAudioDevice(_device);
        AudioSpec want = new()
        {
            Freq     = (int)rate,
            Format   = Sdl.AudioS16Lsb,
            Channels = chan,
            Samples  = 512,
        };
        AudioSpec have;
        _device = sdl.OpenAudioDevice((byte*)null, 0, &want, &have, 0);
        if (_device > 0) sdl.PauseAudioDevice(_device, 0);
        _lastRate = rate;
        _lastChan = chan;
    }

    private void Drain()
    {
        var sdl = _sdl;
        if (sdl == null || _device == 0) return;
        if (!_ctrl.IsPlaying || _ctrl.BufLength == 0) return;

        uint gen = _ctrl.WriteGeneration;
        if (gen == _lastQueuedGen) return;

        // Apply backpressure: don't let the queue grow unboundedly. The guest
        // will spin on Ctrl != 0 until we clear it, which keeps it in lock-step.
        uint queued = sdl.GetQueuedAudioSize(_device);
        if (queued >= 8192) return;

        _lastQueuedGen = gen;
        uint start = _ctrl.BufStart;
        uint len   = Math.Min(_ctrl.BufLength, (uint)_buf.Length - start);
        if (len > 0)
            sdl.QueueAudio(_device, _buf.RawPtr + start, len);
        _ctrl.Ctrl = 0;
    }
}
