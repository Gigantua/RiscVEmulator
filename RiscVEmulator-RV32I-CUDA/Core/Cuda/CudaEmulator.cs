using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// GPU-backed RV32I emulator. The CUDA core (<c>rv32i_cuda.dll</c>) is a pure
    /// RV32I CPU: it has ONE flat memory buffer and every load/store/fetch is just
    /// <c>mem[addr]</c> — it knows nothing of memory regions or MMIO devices.
    ///
    /// All "devices" are therefore plain memory the host fills and drains between
    /// launches: this class lays out RAM, the framebuffer, the PCM buffer and a set
    /// of device register pages inside the one flat buffer, then before each launch
    /// stages inputs (timer, RTC, keyboard, mouse) into those memory cells and after
    /// each launch drains outputs (UART ring, framebuffer, audio, MIDI, exit). The
    /// timer interrupt is delivered by setting the MTIP pending bit in the core's
    /// architectural trap-frame page. The CPU never sees any of this — it only ran
    /// RV32I against memory.
    ///
    /// State is per-core, indexed by <see cref="CoreId"/>.
    /// </summary>
    public sealed unsafe class CudaEmulator : IEmulator, IDisposable
    {
        private const string Lib = "rv32i_cuda";

        [DllImport(Lib)] private static extern int  cuda_rv32i_init(int nCores, uint memBytes);
        [DllImport(Lib)] private static extern int  cuda_rv32i_write_mem(int core, byte[] src, uint off, uint len);
        [DllImport(Lib)] private static extern int  cuda_rv32i_read_mem(int core, byte[] dst, uint off, uint len);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_reg(int core, int i, uint v);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_entry(int core, uint pc);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_halted(int core, int v);
        [DllImport(Lib)] private static extern int  cuda_rv32i_step_all(int budget);
        [DllImport(Lib)] private static extern void cuda_rv32i_shutdown();
        // rvcud: RV32I→CUDA-uarch translator. set_code translates a code image (base..base+len)
        // into a uop stream once; step_all then runs that stream. Same per-core state/memory as the
        // per-instruction kernel, so all device staging/draining is unchanged.
        [DllImport(Lib)] private static extern int  cuda_rvcud_set_code(byte[] src, uint len, uint baseAddr, uint entry);
        [DllImport(Lib)] private static extern int  cuda_rvcud_step_all(int budget);
        [DllImport(Lib)] private static extern ulong cuda_rvcud_iters();
        public ulong RvcudIters => cuda_rvcud_iters();

        // ── Guest memory map (host-side device layout inside the flat buffer) ──
        // The core is oblivious to all of these; they are just memory addresses the
        // guest and host agree on. Must match the guest runtime / MEMORY_MAP.md.
        private const uint CLINT_MTIME = 0x0200BFF8;      // host-advanced timer the guest polls
        // UART output ring (host-drained): head counter + 2 KiB ring, all inside the
        // UART page (0x10000000..0x10001000). The guest's putchar appends to it.
        private const uint UART_HEAD = 0x10000000;
        private const uint UART_RING = 0x10000800;
        private const uint UART_MASK = 0x7FF;
        private const uint KBD_BASE   = 0x10001000;
        private const uint MOUSE_BASE = 0x10002000;
        private const uint RTC_BASE   = 0x10003000;
        private const uint MIDI_BASE  = 0x10005000;
        private const uint FB_BASE    = 0x20000000;
        private const uint DISP_BASE  = 0x20100000;
        private const uint PCM_BASE   = 0x30000000;
        private const uint AUDIO_BASE = 0x30100000;
        private const uint EXIT_BASE  = 0x40000000;
        private const uint EXIT_SENTINEL = 0xFFFFFFFFu;
        // Flat buffer spans [0, EXIT_BASE + a page): RAM at the bottom, devices at
        // their fixed high addresses. ~1 GiB of device VRAM per core (the address
        // map is sparse but a flat buffer must be contiguous — the cost of a CPU
        // with zero region knowledge).
        private const uint FlatBytes = EXIT_BASE + 0x1000u;

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
        private readonly Stopwatch _clock = Stopwatch.StartNew();
        private readonly int _fbBytes;
        private readonly int _pcmBytes;
        private IntPtr _audioMirror;
        private byte[] _pcmTmp = new byte[4096];     // PCM slice scratch (grown on demand)
        private uint   _midiRd;                      // host read cursor into the guest MIDI ring
        private readonly byte[] _midiBuf = new byte[128];
        private const ulong TimebaseHz = 60_000_000UL;

        // Scratch for cell-level reconcile copies.
        private readonly byte[] _b4  = new byte[4];
        private readonly byte[] _b32 = new byte[32];
        private readonly byte[] _uartBuf = new byte[UART_MASK + 1];
        private uint _uartTail;

        private ulong _mtime;

        private bool _committed, _halted, _disposed;
        private int  _exitCode;

        public bool DeterministicTime { get; set; }
        private ulong _totalSteps;

        public Action<char>? OutputHandler { get; set; }
        public Action<uint, uint>? OnMidi { get; set; }
        public bool IsHalted => _halted;
        public int  ExitCode => _exitCode;

        // When set BEFORE CommitImage(), run on the rvcud translated kernel instead of the
        // per-instruction one. The guest code [0, _codeHi) is translated to a uop stream once.
        public bool UseRvcud { get; set; }
        private uint _entry;       // captured by LoadElf / SetEntry — rvcud needs it at commit time
        private uint _codeHi;      // end of the read-only code span — the bytes rvcud translates

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

            int rc = cuda_rv32i_init(nCores, FlatBytes);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_rv32i_init failed (CUDA error {rc})");

            Framebuffer = new FramebufferDevice(fbWidth, fbHeight);
            Display     = new DisplayControlDevice(Framebuffer);
            if (enableMidi) Midi = new MidiDevice();

            _audioMirror = Marshal.AllocHGlobal(pcmBytes);
            new Span<byte>((void*)_audioMirror, pcmBytes).Clear();
            AudioBuffer.Bind((byte*)_audioMirror);
        }

        // ── Image loading ────────────────────────────────────────────────────
        public uint LoadElf(byte[] elf)
        {
            _entry = ElfLoader.Load(elf, new ArrayBus(_image));
            var (_, hi) = ElfLoader.ReadOnlyCodeSpan(elf);   // rvcud translates [0, hi) — covers all code
            _codeHi = hi;
            return _entry;
        }
        public void LoadBytes(uint addr, byte[] data) => Array.Copy(data, 0, _image, (int)addr, data.Length);

        // The per-instruction kernel fetches live from guest RAM, so it needs no code setup beyond
        // the image already written to RAM. The rvcud kernel runs a uop stream, which must be built
        // once from the guest code (cuda_rvcud_set_code) before stepping.
        private void SetupCode()
        {
            if (!UseRvcud) return;
            int rc = cuda_rvcud_set_code(_image, _codeHi != 0 ? _codeHi : (uint)_image.Length, 0, _entry);
            if (rc != 0) throw new InvalidOperationException($"cuda_rvcud_set_code failed (CUDA error {rc})");
        }

        public void CommitImage()
        {
            cuda_rv32i_write_mem(CoreId, _image, 0, (uint)_image.Length);
            ArmExit(CoreId);
            SetupCode();   // one shared code image / uop stream for all cores
            _committed = true;
        }

        public void CommitImageToAllCores(uint sp, uint entry)
        {
            _entry = entry;
            for (int c = 0; c < NumCores; c++)
            {
                cuda_rv32i_write_mem(c, _image, 0, (uint)_image.Length);
                cuda_rv32i_set_reg(c, 2, sp);
                cuda_rv32i_set_entry(c, entry);
                ArmExit(c);
            }
            SetupCode();
            _committed = true;
        }

        // Shared read-only image is no longer a core feature (the core has no
        // region knowledge); kept as a no-op for API compatibility.
        public int SharedRoBytes => 0;
        public int CommitSharedRo() => 0;

        public void SetReg(int index, uint value) => cuda_rv32i_set_reg(CoreId, index, value);
        public void SetEntry(uint pc)             { _entry = pc; cuda_rv32i_set_entry(CoreId, pc); }

        public void ReadFramebuffer(byte[] dst, int len) => cuda_rv32i_read_mem(CoreId, dst, FB_BASE, (uint)len);
        public void ReadRam(byte[] dst, uint off, uint len) => cuda_rv32i_read_mem(CoreId, dst, off, len);

        public void ReadPcm(byte[] dst, int len)
        {
            var h = GCHandle.Alloc(dst, GCHandleType.Pinned);
            try
            {
                // read_mem expects a byte[]; reuse via a temporary copy path.
                var tmp = new byte[len];
                cuda_rv32i_read_mem(CoreId, tmp, PCM_BASE, (uint)len);
                Marshal.Copy(tmp, 0, h.AddrOfPinnedObject(), len);
            }
            finally { h.Free(); }
        }

        // ── Run ──────────────────────────────────────────────────────────────
        public int StepN(int n)
        {
            if (_halted) return 0;
            if (!_committed) throw new InvalidOperationException("CommitImage() must be called before StepN().");

            _totalSteps += (ulong)n;
            StageInputs();
            int rc = UseRvcud ? cuda_rvcud_step_all(n) : cuda_rv32i_step_all(n);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_{(UseRvcud ? "rvcud" : "rv32i")}_step_all failed (CUDA error {rc})");
            DrainOutputs();

            uint exit = R32(EXIT_BASE);
            if (exit != EXIT_SENTINEL) { _halted = true; _exitCode = (int)(exit & 0x7FFFFFFF); cuda_rv32i_set_halted(CoreId, 1); }
            return n;
        }

        public void SetHalted(bool value)
        {
            cuda_rv32i_set_halted(CoreId, value ? 1 : 0);
            _halted = value;
        }

        // ── Memory-cell helpers (host ↔ flat device buffer) ──────────────────
        private uint R32(uint addr) { cuda_rv32i_read_mem(CoreId, _b4, addr, 4); return BitConverter.ToUInt32(_b4, 0); }
        private void W32(uint addr, uint v) { BitConverter.TryWriteBytes(_b4, v); cuda_rv32i_write_mem(CoreId, _b4, addr, 4); }
        private void ArmExit(int core)
        {
            BitConverter.TryWriteBytes(_b4, EXIT_SENTINEL);
            cuda_rv32i_write_mem(core, _b4, EXIT_BASE, 4);
        }

        // ── Stage inputs (host → memory) before a launch ─────────────────────
        private void StageInputs()
        {
            StageTimers();

            // Keyboard: a 1-deep mailbox with guest ack. The guest clears the has-cell after consuming
            // an event (poll_keyboard writes KB_STATUS = 0), so stage the next one ONLY when the cell
            // reads 0 — unconditionally re-staging every launch overwrote any event the guest hadn't
            // polled within that one batch (the guest polls once per rendered frame), silently dropping
            // most keys. Unconsumed events now persist across launches; bursts buffer in the host FIFO.
            if (R32(KBD_BASE) == 0 && Keyboard.Read(0x00, 4) != 0)
            {
                uint sc = Keyboard.Read(0x04, 4);                // pops the host FIFO
                BitConverter.TryWriteBytes(_b32.AsSpan(0), 1u);
                BitConverter.TryWriteBytes(_b32.AsSpan(4), sc);
                cuda_rv32i_write_mem(CoreId, _b32, KBD_BASE, 8);
            }
            BitConverter.TryWriteBytes(_b32.AsSpan(0), Keyboard.Read(0x08, 4));   // modifiers: always fresh
            cuda_rv32i_write_mem(CoreId, _b32, KBD_BASE + 8, 4);

            // Mouse: stage accumulated deltas/buttons (the reads clear the C# side).
            int  dx  = (int)Mouse.Read(0x04, 4);
            int  dy  = (int)Mouse.Read(0x08, 4);
            uint btn = Mouse.Read(0x0C, 4);
            uint mhas = (dx != 0 || dy != 0 || btn != 0) ? 1u : 0u;
            BitConverter.TryWriteBytes(_b32.AsSpan(0),  mhas);
            BitConverter.TryWriteBytes(_b32.AsSpan(4),  (uint)dx);
            BitConverter.TryWriteBytes(_b32.AsSpan(8),  (uint)dy);
            BitConverter.TryWriteBytes(_b32.AsSpan(12), btn);
            cuda_rv32i_write_mem(CoreId, _b32, MOUSE_BASE, 16);
        }

        private void StageTimers()
        {
            ulong us, ms, ep; uint sec, subus;
            if (DeterministicTime)
            {
                us = _totalSteps / 3UL; ms = us / 1000UL; sec = (uint)(us / 1_000_000UL);
                ep = 1_700_000_000UL + sec; _mtime = us * (TimebaseHz / 1_000_000UL); subus = (uint)(us % 1_000_000UL);
            }
            else
            {
                us = (ulong)_clock.Elapsed.TotalMicroseconds; ms = (ulong)_clock.ElapsedMilliseconds;
                ep = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds(); sec = (uint)_clock.Elapsed.TotalSeconds;
                ulong ticks = (ulong)_clock.ElapsedTicks, freq = (ulong)Stopwatch.Frequency;
                _mtime = (ticks / freq) * TimebaseHz + (ticks % freq) * TimebaseHz / freq; subus = (uint)(us % 1_000_000UL);
            }
            // RTC page (8 words) and CLINT mtime.
            BitConverter.TryWriteBytes(_b32.AsSpan(0),  (uint)us);  BitConverter.TryWriteBytes(_b32.AsSpan(4),  (uint)(us >> 32));
            BitConverter.TryWriteBytes(_b32.AsSpan(8),  (uint)ms);  BitConverter.TryWriteBytes(_b32.AsSpan(12), (uint)(ms >> 32));
            BitConverter.TryWriteBytes(_b32.AsSpan(16), (uint)ep);  BitConverter.TryWriteBytes(_b32.AsSpan(20), (uint)(ep >> 32));
            BitConverter.TryWriteBytes(_b32.AsSpan(24), sec);       BitConverter.TryWriteBytes(_b32.AsSpan(28), subus);
            cuda_rv32i_write_mem(CoreId, _b32, RTC_BASE, 32);

            BitConverter.TryWriteBytes(_b32.AsSpan(0), (uint)_mtime);
            BitConverter.TryWriteBytes(_b32.AsSpan(4), (uint)(_mtime >> 32));
            cuda_rv32i_write_mem(CoreId, _b32, CLINT_MTIME, 8);
            // No timer interrupt: the core has no trap unit. Guests poll the mtime
            // / RTC memory cells directly.
        }

        // ── Drain outputs (memory → host) after a launch ─────────────────────
        private void DrainOutputs()
        {
            // UART ring → console / OutputHandler.
            uint head = R32(UART_HEAD);
            var sink = OutputHandler;
            while (_uartTail != head)
            {
                uint avail = head - _uartTail;
                if (avail > UART_MASK + 1) { _uartTail = head - (UART_MASK + 1); avail = UART_MASK + 1; }  // overran the ring
                uint start = _uartTail & UART_MASK;
                uint chunk = Math.Min(avail, (UART_MASK + 1) - start);
                cuda_rv32i_read_mem(CoreId, _uartBuf, UART_RING + start, chunk);
                if (sink != null) for (uint i = 0; i < chunk; i++) sink((char)_uartBuf[i]);
                _uartTail += chunk;
            }

            // MIDI: lossless ring (guest writes entries at +0x20.. and its write index at +0x10; the
            // host keeps its own read cursor). The old single-cell drain kept only the LAST message
            // per launch — the guest's tick loop overwrote the cell per message, so chords and
            // note-offs vanished. Guests that never touch the ring fall back to the legacy cell.
            if (Midi != null || OnMidi != null)
            {
                uint wr = R32(MIDI_BASE + 0x10);
                if (wr != 0)
                {
                    if (wr - _midiRd > 32) _midiRd = wr - 32;            // ring overrun (32 entries)
                    if (wr != _midiRd)
                    {
                        cuda_rv32i_read_mem(CoreId, _midiBuf, MIDI_BASE + 0x20, 128);
                        for (; _midiRd != wr; _midiRd++)
                        {
                            uint m = BitConverter.ToUInt32(_midiBuf, (int)((_midiRd & 31) * 4)) & 0x00FFFFFFu;
                            if (m == 0) continue;
                            OnMidi?.Invoke(0x04, m);
                            Midi?.Write(0x04, 4, m);
                        }
                    }
                }
                else
                {
                    uint m = R32(MIDI_BASE + 0x04);                      // legacy single-cell guests
                    if (m != 0)
                    {
                        OnMidi?.Invoke(0x04, m & 0x00FFFFFFu);
                        Midi?.Write(0x04, 4, m & 0x00FFFFFFu);
                        W32(MIDI_BASE + 0x04, 0);   // consume
                    }
                }
            }

            // Audio: snapshot the control page; on a guest-submitted buffer (ctrl bit 0) copy the PCM
            // slice, bump the playback generation THROUGH the device Write path (SdlWindow keys on
            // WriteGeneration — the old property assignment bypassed it, so nothing was ever queued),
            // and ACK by clearing the guest's ctrl cell (copy_audio waits for 0 before submitting the
            // next buffer — without the ack exactly one 46 ms buffer ever played).
            cuda_rv32i_read_mem(CoreId, _b32, AUDIO_BASE, 32);
            uint actrl = BitConverter.ToUInt32(_b32, 0);
            AudioControl.SampleRate = BitConverter.ToUInt32(_b32, 8);
            AudioControl.Channels   = BitConverter.ToUInt32(_b32, 12);
            AudioControl.BitDepth   = BitConverter.ToUInt32(_b32, 16);
            AudioControl.BufStart   = BitConverter.ToUInt32(_b32, 20);
            AudioControl.BufLength  = BitConverter.ToUInt32(_b32, 24);
            AudioControl.Position   = BitConverter.ToUInt32(_b32, 28);
            if ((actrl & 1) != 0)
            {
                uint blen = Math.Min(AudioControl.BufLength, (uint)_pcmBytes);
                uint boff = Math.Min(AudioControl.BufStart, (uint)_pcmBytes - blen);
                if (blen > 0)
                {
                    if (_pcmTmp.Length < blen) _pcmTmp = new byte[blen];
                    cuda_rv32i_read_mem(CoreId, _pcmTmp, PCM_BASE + boff, blen);
                    Marshal.Copy(_pcmTmp, 0, _audioMirror + (int)boff, (int)blen);
                }
                AudioControl.Write(0x00, 4, actrl);   // sets Ctrl + bumps WriteGeneration
                W32(AUDIO_BASE, 0);                   // ack → the guest may submit the next buffer
            }

            // Display: vsync / mode / fbaddr, then present.
            uint vsync  = R32(DISP_BASE + 0x0C);
            uint fbAddr = R32(DISP_BASE + 0x1C);
            if (vsync != 0) { Display.Write(0x0C, 4, vsync); W32(DISP_BASE + 0x0C, 0); }
            int fbLen = Math.Min(_fbBytes, Framebuffer.PresentedPixels.Length);
            if (fbAddr != 0 && fbAddr < (uint)RamBytes)
                cuda_rv32i_read_mem(CoreId, Framebuffer.PresentedPixels, fbAddr, (uint)fbLen);
            else
                cuda_rv32i_read_mem(CoreId, Framebuffer.PresentedPixels, FB_BASE, (uint)fbLen);
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
