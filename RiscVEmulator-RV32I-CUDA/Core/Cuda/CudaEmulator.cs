using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// GPU-backed RV32I emulator. The CPU core runs on the GPU via
    /// <c>rv32i_cuda.dll</c>; this class is the C# shell that loads the guest
    /// image, drives launches and reconciles peripherals.
    ///
    /// Memory model: RAM, framebuffer and PCM are pure device memory
    /// (<c>cudaMalloc</c>) — they never migrate, and the host reaches them only
    /// through explicit copies between launches (image commit H2D; framebuffer /
    /// audio readback D2H). The peripheral register page and the trap-frame page
    /// are <em>unified</em> (managed) memory: the device uses them natively, and
    /// the host reconciles them between launches. The trap unit (M-mode +
    /// trap-frame page) lives device-side; the device compares guest
    /// <c>mtime</c>/<c>mtimecmp</c> itself, so a timer interrupt is taken at the
    /// next launch boundary after <c>mtime</c> reaches <c>mtimecmp</c>
    /// (host advances <c>mtime</c> in <see cref="StepN"/>). Live mid-launch
    /// interrupts are not possible under WDDM's limited unified memory.
    ///
    /// State is per-core, indexed by <see cref="CoreId"/>.
    /// </summary>
    public sealed unsafe class CudaEmulator : IEmulator, IDisposable
    {
        private const string Lib = "rv32i_cuda";

        [DllImport(Lib)] private static extern int  cuda_rv32i_init(int nCores, uint ramSize, uint fbW, uint fbH, uint pcmBytes);
        [DllImport(Lib)] private static extern int  cuda_rv32i_load_ram(int core, byte[] src, uint off, uint len);
        [DllImport(Lib)] private static extern int  cuda_rv32i_read_ram(int core, byte[] dst, uint off, uint len);
        [DllImport(Lib)] private static extern int  cuda_rv32i_read_fb(int core, byte[] dst, uint len);
        [DllImport(Lib)] private static extern int  cuda_rv32i_read_pcm(int core, IntPtr dst, uint len);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_reg(int core, int i, uint v);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_entry(int core, uint pc);
        [DllImport(Lib)] private static extern uint cuda_rv32i_get_pc(int core);
        [DllImport(Lib)] private static extern int  cuda_rv32i_is_halted(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_halted(int core, int v);
        [DllImport(Lib)] private static extern int  cuda_rv32i_exitcode(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_mtime(int core, uint lo, uint hi);
        [DllImport(Lib)] private static extern int  cuda_rv32i_step_all(int budget);
        [DllImport(Lib)] private static extern void cuda_rv32i_prof_reset();
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_prof_ptr();
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_jump_ptr();
        [DllImport(Lib)] private static extern int  cuda_rv32i_profile(int budget);
        [DllImport(Lib)] private static extern int  cuda_rv32i_uart_drain(int core, byte[] dst, int maxlen);
        [DllImport(Lib)] private static extern void cuda_rv32i_kbd_feed(int core, uint entry);
        [DllImport(Lib)] private static extern void cuda_rv32i_kbd_set_mod(int core, uint mod);
        [DllImport(Lib)] private static extern void cuda_rv32i_mouse_feed(int core, int dx, int dy, uint buttons);
        [DllImport(Lib)] private static extern int  cuda_rv32i_midi_drain(int core, uint[] dst, int maxlen);
        [DllImport(Lib)] private static extern void cuda_rv32i_audio_snapshot(int core, uint[] out8);
        [DllImport(Lib)] private static extern uint cuda_rv32i_display_take_vsync(int core);
        [DllImport(Lib)] private static extern uint cuda_rv32i_display_fbaddr(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_time(int core, uint usLo, uint usHi, uint msLo, uint msHi, uint epLo, uint epHi, uint sec, uint subus);
        [DllImport(Lib)] private static extern void cuda_rv32i_shutdown();

        // ── Reusable host-side peripheral endpoints (SDL/console facing) ─────
        public KeyboardDevice       Keyboard     { get; } = new();
        public MouseDevice          Mouse        { get; } = new();
        public MidiDevice?          Midi         { get; }
        public AudioBufferDevice    AudioBuffer  { get; } = new();
        public AudioControlDevice   AudioControl { get; } = new();
        public FramebufferDevice    Framebuffer  { get; }
        public DisplayControlDevice Display       { get; }

        public int  CoreId   { get; }
        public int  NumCores { get; }
        public int  RamBytes { get; }

        private readonly byte[] _image;
        private readonly byte[] _drainBuf = new byte[8192];
        private readonly uint[] _midiBuf  = new uint[1024];
        private readonly uint[] _au       = new uint[8];
        private readonly Stopwatch _clock = Stopwatch.StartNew();
        private readonly int _fbBytes;
        private readonly int _pcmBytes;
        private IntPtr _audioMirror;       // host copy of device PCM (SDL reads this)
        private uint   _lastAudioGen = unchecked((uint)-1);
        private const ulong TimebaseHz = 60_000_000UL;

        private bool _committed, _halted, _disposed;
        private int  _exitCode;

        public Action<char>? OutputHandler { get; set; }
        /// <summary>Optional raw MIDI sink (offset, value) — set instead of/alongside
        /// <see cref="Midi"/> to observe messages without winmm playback (used by tests).</summary>
        public Action<uint, uint>? OnMidi { get; set; }
        public bool IsHalted => _halted;
        public int  ExitCode => _exitCode;
        public uint PC       => cuda_rv32i_get_pc(CoreId);

        public CudaEmulator(int ramBytes, int fbWidth = 320, int fbHeight = 200,
                            int pcmBytes = 1 << 20, bool enableMidi = false,
                            int nCores = 1, int coreId = 0)
        {
            NumCores = nCores;
            CoreId   = coreId;
            RamBytes = ramBytes;
            _image   = new byte[ramBytes];
            _fbBytes  = fbWidth * fbHeight * 4;
            _pcmBytes = pcmBytes;

            int rc = cuda_rv32i_init(nCores, (uint)ramBytes, (uint)fbWidth, (uint)fbHeight, (uint)pcmBytes);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_rv32i_init failed (CUDA error {rc})");

            Framebuffer = new FramebufferDevice(fbWidth, fbHeight);
            Display     = new DisplayControlDevice(Framebuffer);
            if (enableMidi) Midi = new MidiDevice();

            // Host mirror of the device PCM buffer (SDL drains from here; the
            // real PCM is device memory copied D2H between launches).
            _audioMirror = Marshal.AllocHGlobal(pcmBytes);
            new Span<byte>((void*)_audioMirror, pcmBytes).Clear();
            AudioBuffer.Bind((byte*)_audioMirror);
        }

        // ── Image loading (host side, before the first launch) ───────────────

        public uint LoadElf(byte[] elf) => ElfLoader.Load(elf, new ArrayBus(_image));
        public void LoadBytes(uint addr, byte[] data) => Array.Copy(data, 0, _image, (int)addr, data.Length);

        public void CommitImage()
        {
            cuda_rv32i_load_ram(CoreId, _image, 0, (uint)_image.Length);
            _committed = true;
        }

        /// <summary>Broadcast the staged RAM image to ALL cores and set their sp/entry
        /// — used to run N independent copies of the same guest (throughput demo).</summary>
        public void CommitImageToAllCores(uint sp, uint entry)
        {
            for (int c = 0; c < NumCores; c++)
            {
                cuda_rv32i_load_ram(c, _image, 0, (uint)_image.Length);
                cuda_rv32i_set_reg(c, 2, sp);
                cuda_rv32i_set_entry(c, entry);
            }
            _committed = true;
        }

        public void SetReg(int index, uint value) => cuda_rv32i_set_reg(CoreId, index, value);
        public void SetEntry(uint pc)             => cuda_rv32i_set_entry(CoreId, pc);

        /// <summary>Copy the device framebuffer (or PCM) to a host buffer (between launches).</summary>
        public void ReadFramebuffer(byte[] dst, int len) => cuda_rv32i_read_fb(CoreId, dst, (uint)len);
        public void ReadPcm(byte[] dst, int len)
        {
            var h = GCHandle.Alloc(dst, GCHandleType.Pinned);
            try { cuda_rv32i_read_pcm(CoreId, h.AddrOfPinnedObject(), (uint)len); }
            finally { h.Free(); }
        }

        // ── ISA profiler (single-guest dynamic opcode/pair histogram) ─────────
        public void ProfReset() => cuda_rv32i_prof_reset();
        public int  Profile(int budget) => cuda_rv32i_profile(budget);
        public ulong[] ProfRead()
        {
            const int n = 1024 + 8;
            var t = new long[n];
            Marshal.Copy(cuda_rv32i_prof_ptr(), t, 0, n);
            var u = new ulong[n];
            for (int i = 0; i < n; i++) u[i] = (ulong)t[i];
            return u;
        }
        public ulong[] JumpRead()
        {
            const int n = 3 * 64;
            var t = new long[n];
            Marshal.Copy(cuda_rv32i_jump_ptr(), t, 0, n);
            var u = new ulong[n];
            for (int i = 0; i < n; i++) u[i] = (ulong)t[i];
            return u;
        }

        // ── Run ──────────────────────────────────────────────────────────────

        public int StepN(int n)
        {
            if (_halted) return 0;
            if (!_committed) throw new InvalidOperationException("CommitImage() must be called before StepN().");

            StageInputs();
            int rc = cuda_rv32i_step_all(n);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_rv32i_step_all failed (CUDA error {rc})");
            DrainOutputs();

            _halted = cuda_rv32i_is_halted(CoreId) != 0;
            if (_halted) _exitCode = cuda_rv32i_exitcode(CoreId);
            return n;
        }

        public void SetHalted(bool value)
        {
            cuda_rv32i_set_halted(CoreId, value ? 1 : 0);
            _halted = value;
        }

        // ── Reconcile: host → managed (before launch) ────────────────────────

        private void StageInputs()
        {
            // Keyboard FIFO: drain the C# device's queue into the managed FIFO.
            while (Keyboard.Read(0x00, 4) != 0)
                cuda_rv32i_kbd_feed(CoreId, Keyboard.Read(0x04, 4));
            cuda_rv32i_kbd_set_mod(CoreId, Keyboard.Read(0x08, 4));

            // Mouse: pull accumulated deltas (read clears the C# side) + buttons.
            int  dx  = (int)Mouse.Read(0x04, 4);
            int  dy  = (int)Mouse.Read(0x08, 4);
            uint btn = Mouse.Read(0x0C, 4);
            if (dx != 0 || dy != 0 || btn != 0)
                cuda_rv32i_mouse_feed(CoreId, dx, dy, btn);

            // Wall-clock time for the RTC.
            ulong us  = (ulong)_clock.Elapsed.TotalMicroseconds;
            ulong ms  = (ulong)_clock.ElapsedMilliseconds;
            ulong ep  = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            uint  sec = (uint)_clock.Elapsed.TotalSeconds;
            cuda_rv32i_set_time(CoreId, (uint)us, (uint)(us >> 32), (uint)ms, (uint)(ms >> 32),
                                (uint)ep, (uint)(ep >> 32), sec, (uint)(us % 1_000_000UL));

            // CLINT mtime (drives the device-side timer-interrupt compare). Set
            // between launches — a timer interrupt is taken at the next launch
            // boundary after mtime crosses the guest's mtimecmp.
            ulong ticks = (ulong)_clock.ElapsedTicks;
            ulong freq  = (ulong)Stopwatch.Frequency;
            ulong mtime = (ticks / freq) * TimebaseHz + (ticks % freq) * TimebaseHz / freq;
            cuda_rv32i_set_mtime(CoreId, (uint)mtime, (uint)(mtime >> 32));
        }

        // ── Reconcile: managed → host (after launch) ─────────────────────────

        private void DrainOutputs()
        {
            // UART TX → console / OutputHandler.
            var h = OutputHandler;
            int got;
            while ((got = cuda_rv32i_uart_drain(CoreId, _drainBuf, _drainBuf.Length)) > 0)
            {
                if (h != null) for (int i = 0; i < got; i++) h((char)_drainBuf[i]);
                if (got < _drainBuf.Length) break;
            }

            // MIDI ring → OnMidi sink and/or the MIDI device (winmm playback).
            // Always drained so the ring can't overflow even with no consumer.
            if (Midi != null || OnMidi != null)
            {
                int mn;
                while ((mn = cuda_rv32i_midi_drain(CoreId, _midiBuf, _midiBuf.Length)) > 0)
                {
                    for (int i = 0; i < mn; i++)
                    {
                        uint off = _midiBuf[i] >> 24;
                        uint val = _midiBuf[i] & 0x00FFFFFFu;
                        OnMidi?.Invoke(off, val);
                        Midi?.Write(off, 4, val);
                    }
                    if (mn < _midiBuf.Length) break;
                }
            }

            // Audio control snapshot → AudioControlDevice; copy PCM on new data.
            cuda_rv32i_audio_snapshot(CoreId, _au);
            AudioControl.Ctrl       = _au[0];
            AudioControl.SampleRate = _au[1];
            AudioControl.Channels   = _au[2];
            AudioControl.BitDepth   = _au[3];
            AudioControl.BufStart   = _au[4];
            AudioControl.BufLength  = _au[5];
            AudioControl.Position   = _au[6];
            uint gen = _au[7];
            if (gen != _lastAudioGen)
            {
                _lastAudioGen = gen;
                cuda_rv32i_read_pcm(CoreId, _audioMirror, (uint)_pcmBytes);
                AudioControl.WriteGeneration++;
            }

            // Framebuffer present: copy the device FB (or the in-RAM FB the guest
            // pointed at) D2H into the host PresentedPixels that SDL reads.
            uint vsync = cuda_rv32i_display_take_vsync(CoreId);
            uint fbAddr = cuda_rv32i_display_fbaddr(CoreId);
            int  fbLen = Math.Min(_fbBytes, Framebuffer.PresentedPixels.Length);
            if (fbAddr != 0 && fbAddr < (uint)RamBytes)
                cuda_rv32i_read_ram(CoreId, Framebuffer.PresentedPixels, fbAddr, (uint)fbLen);
            else
                cuda_rv32i_read_fb(CoreId, Framebuffer.PresentedPixels, (uint)fbLen);
        }

        public void Dispose()
        {
            if (_disposed) return;
            cuda_rv32i_shutdown();
            if (_audioMirror != IntPtr.Zero) { Marshal.FreeHGlobal(_audioMirror); _audioMirror = IntPtr.Zero; }
            Midi?.Dispose();
            _disposed = true;
            GC.SuppressFinalize(this);
        }

        // ── Minimal IMemoryBus over a byte[] so ElfLoader can build the image ─
        private sealed class ArrayBus : IMemoryBus
        {
            private readonly byte[] _ram;
            public ArrayBus(byte[] ram) => _ram = ram;
            public int RamSize => _ram.Length;
            public System.Collections.Generic.IReadOnlyList<IPeripheral> Peripherals
                => System.Array.Empty<IPeripheral>();
            public byte   ReadByte(uint a)     => _ram[a];
            public ushort ReadHalfWord(uint a) => (ushort)(_ram[a] | (_ram[a + 1] << 8));
            public uint   ReadWord(uint a)     =>
                (uint)(_ram[a] | (_ram[a + 1] << 8) | (_ram[a + 2] << 16) | (_ram[a + 3] << 24));
            public void WriteByte(uint a, byte v)      => _ram[a] = v;
            public void WriteHalfWord(uint a, ushort v){ _ram[a] = (byte)v; _ram[a + 1] = (byte)(v >> 8); }
            public void WriteWord(uint a, uint v)
            {
                _ram[a]     = (byte)v;        _ram[a + 1] = (byte)(v >> 8);
                _ram[a + 2] = (byte)(v >> 16); _ram[a + 3] = (byte)(v >> 24);
            }
            public void Load(uint address, byte[] src, int srcOffset, int length)
                => Array.Copy(src, srcOffset, _ram, (int)address, length);
        }
    }
}
