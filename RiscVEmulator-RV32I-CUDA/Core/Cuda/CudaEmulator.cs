using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// GPU-backed RV32I emulator. The CPU core runs on the GPU in a single
    /// thread (<c>__launch_bounds__(1,1)</c>) via <c>rv32i_cuda.dll</c>; this
    /// class is the C# shell that loads the guest image, drives batch-
    /// synchronous launches and reconciles peripherals between them.
    ///
    /// Memory model: all guest buffers are CUDA managed memory owned by the
    /// DLL. Windows/WDDM forbids host access to managed memory while a kernel
    /// runs, so this class only touches it between launches. <see cref="StepN"/>
    /// stages input (keyboard/mouse/time) into the managed peripheral page,
    /// launches the kernel for a budget of steps, synchronises, then drains
    /// output (UART/MIDI/audio/framebuffer).
    ///
    /// Peripheral behaviour is split: the device half lives in
    /// <c>rv32i_cuda.cu</c> (plain managed-memory reads/writes); the host half
    /// reuses the existing <see cref="Core.Peripherals"/> device objects as the
    /// SDL/console-facing endpoints. SDL only ever reads host-side snapshots
    /// (<see cref="FramebufferDevice.PresentedPixels"/>, the mirrored audio
    /// buffer) — never managed memory directly.
    ///
    /// State is per-core, indexed by <see cref="CoreId"/>. Today nCores = 1;
    /// the DLL's arrays generalise to many cores for a future fan-out.
    /// </summary>
    public sealed unsafe class CudaEmulator : IEmulator, IDisposable
    {
        private const string Lib = "rv32i_cuda";

        [DllImport(Lib)] private static extern int  cuda_rv32i_init(int nCores, uint ramSize, uint fbW, uint fbH, uint pcmBytes);
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_ram_ptr(int core);
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_fb_ptr(int core);
        [DllImport(Lib)] private static extern uint cuda_rv32i_fb_bytes(int core);
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_pcm_ptr(int core);
        [DllImport(Lib)] private static extern uint cuda_rv32i_pcm_bytes(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_reg(int core, int i, uint v);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_entry(int core, uint pc);
        [DllImport(Lib)] private static extern uint cuda_rv32i_get_pc(int core);
        [DllImport(Lib)] private static extern int  cuda_rv32i_is_halted(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_halted(int core, int v);
        [DllImport(Lib)] private static extern int  cuda_rv32i_exitcode(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_mtip(int core, int level);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_meip(int core, int level);
        [DllImport(Lib)] private static extern int  cuda_rv32i_set_code(byte[] data, uint lo, uint len);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_block(int b);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_prefetch(int on);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_pico(int on);
        [DllImport(Lib)] private static extern uint cuda_rv32i_corestate_bytes();
        [DllImport(Lib)] private static extern void cuda_rv32i_set_l2advise(int on);
        [DllImport(Lib)] private static extern int  cuda_rv32i_get_l2advise();
        [DllImport(Lib)] private static extern int  cuda_rv32i_set_fastpath(int on);
        [DllImport(Lib)] private static extern int  cuda_rv32i_step_all(long budget);
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_state_ptr();
        [DllImport(Lib)] private static extern IntPtr cuda_rv32i_mem_ptr();
        [DllImport(Lib)] private static extern int  cuda_rv32i_ncores();
        [DllImport(Lib)] private static extern int  cuda_rv32i_uart_drain(int core, byte[] dst, int maxlen);
        [DllImport(Lib)] private static extern void cuda_rv32i_uart_feed(int core, byte[] src, int len);
        [DllImport(Lib)] private static extern void cuda_rv32i_kbd_feed(int core, uint entry);
        [DllImport(Lib)] private static extern void cuda_rv32i_kbd_set_mod(int core, uint mod);
        [DllImport(Lib)] private static extern void cuda_rv32i_mouse_feed(int core, int dx, int dy, uint buttons);
        [DllImport(Lib)] private static extern int  cuda_rv32i_midi_drain(int core, uint[] dst, int maxlen);
        [DllImport(Lib)] private static extern void cuda_rv32i_audio_snapshot(int core, uint[] out8);
        [DllImport(Lib)] private static extern void cuda_rv32i_audio_ack(int core);
        [DllImport(Lib)] private static extern uint cuda_rv32i_display_take_vsync(int core);
        [DllImport(Lib)] private static extern uint cuda_rv32i_display_fbaddr(int core);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_time(int core, uint usLo, uint usHi, uint msLo, uint msHi, uint epLo, uint epHi, uint sec, uint subus);
        [DllImport(Lib)] private static extern void cuda_rv32i_set_mtime(int core, uint lo, uint hi);
        [DllImport(Lib)] private static extern ulong cuda_rv32i_mtimecmp(int core);
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
        private IntPtr _audioMirror;       // host copy of managed PCM (SDL reads this)
        private uint   _lastAudioGen = unchecked((uint)-1);
        private const ulong TimebaseHz = 60_000_000UL;

        private bool _committed, _halted, _disposed;
        private int  _exitCode;
        private byte[]? _elf;            // retained to compute the RO code span
        private uint _codeLo, _codeHi;   // shareable read-only span [lo, hi)

        /// <summary>Install the guest's read-only code segment as a single shared,
        /// L2-resident image (Tier 1b): all cores fetch from it, so same-PC lanes
        /// coalesce. Transparent (identical bytes), helps multi-core throughput.</summary>
        public bool UseSharedCode { get; set; }

        /// <summary>Enable the double-buffered shared-memory instruction window
        /// (Tier 3): fetch from on-chip shared memory with the next section
        /// cp.async-prefetched, hiding the ~530-cycle fetch latency. Best for
        /// single-/few-guest latency (e.g. one Doom instance). Implies shared code.</summary>
        public bool EnablePrefetch { get; set; }

        /// <summary>Mott — L2-resident working set. When true (the DLL default),
        /// the CPU working set (CoreState[]/CoreMem[] + each core's RAM/trap/
        /// peripheral page + the shared RO code image) is tagged device-preferred
        /// (the code image additionally <c>ReadMostly</c>) and prefetched to the
        /// GPU before launches, so the dependent-load fetch hits L2 instead of
        /// fault-migrating from host DRAM. Pure placement hint — results are
        /// bit-for-bit identical with it on or off. Set false to force the
        /// original allocation behaviour (A/B baseline).</summary>
        public bool L2Advise
        {
            get => cuda_rv32i_get_l2advise() != 0;
            set => cuda_rv32i_set_l2advise(value ? 1 : 0);
        }

        /// <summary>Translate this guest's code to native CUDA, nvcc-compile it
        /// into a per-guest JIT DLL, and run THAT kernel instead of the
        /// interpreter (set before <see cref="CommitImage"/> /
        /// <see cref="CommitImageToAllCores"/>). The JIT shares the same managed
        /// <c>CoreState[]</c>/<c>CoreMem[]</c> as the interpreter, so all
        /// peripheral reconcile (UART/FB/kbd/…) and traps still work — and the
        /// interpreter remains the fallback for any opcode/target it doesn't
        /// translate (SYSTEM/ECALL, JALR into untranslated addresses) plus the
        /// halt/budget guards keep it hang-safe. If the nvcc build fails, the
        /// emulator silently falls back to the interpreter for the whole run.
        /// </summary>
        public bool UseJit { get; set; }

        /// <summary>PICO state-minimizer: when true (default), the per-core soft-CSR
        /// file is allocated as one contiguous slab so <c>sizeof(CoreState)</c> stays
        /// ~64 B instead of ~16.4 KiB, letting far more cores co-reside at a fixed
        /// VRAM budget (occupancy → latency hidden). Bit-exact either way (CSR[fn] is
        /// the same word). Static because it must be set BEFORE the constructor's
        /// <c>cuda_rv32i_init</c>; set to false to reproduce the baseline layout.</summary>
        public static bool PicoStateMin { get; set; } = true;

        /// <summary>Bytes of densely-packed per-core launch/exit state (<c>sizeof(CoreState)</c>).
        /// Smaller ⇒ more resident cores at fixed VRAM. Reflects <see cref="PicoStateMin"/>.</summary>
        public uint CoreStateBytes => cuda_rv32i_corestate_bytes();

        /// <summary>True if <see cref="UseJit"/> was requested AND the JIT DLL
        /// built+loaded successfully — i.e. StepN is running JIT'd code.</summary>
        public bool JitActive => _jit != null;

        private RvJitRuntime? _jit;

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

            cuda_rv32i_set_pico(PicoStateMin ? 1 : 0);   // must precede init (governs CSR layout)
            int rc = cuda_rv32i_init(nCores, (uint)ramBytes, (uint)fbWidth, (uint)fbHeight, (uint)pcmBytes);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_rv32i_init failed (CUDA error {rc})");

            Framebuffer = new FramebufferDevice(fbWidth, fbHeight);
            Display     = new DisplayControlDevice(Framebuffer);
            if (enableMidi) Midi = new MidiDevice();

            // Host mirror of the audio PCM buffer (SDL drains from here, never
            // from managed memory while a kernel runs).
            _audioMirror = Marshal.AllocHGlobal(pcmBytes);
            new Span<byte>((void*)_audioMirror, pcmBytes).Clear();
            AudioBuffer.Bind((byte*)_audioMirror);
        }

        // ── Image loading (host side, before the first launch) ───────────────

        public uint LoadElf(byte[] elf)
        {
            _elf = elf;
            (_codeLo, _codeHi) = ElfLoader.ReadOnlyCodeSpan(elf);
            return ElfLoader.Load(elf, new ArrayBus(_image));
        }
        public void LoadBytes(uint addr, byte[] data) => Array.Copy(data, 0, _image, (int)addr, data.Length);

        // Install the shared RO code image and/or enable the instruction window,
        // per the UseSharedCode / EnablePrefetch flags. set_code broadcasts to
        // every core in the DLL, so one call suffices regardless of NumCores.
        private void InstallAccel()
        {
            if ((UseSharedCode || EnablePrefetch) && _codeHi > _codeLo)
            {
                byte[] code = _image[(int)_codeLo..(int)_codeHi];
                cuda_rv32i_set_code(code, _codeLo, (uint)code.Length);
            }
            cuda_rv32i_set_prefetch(EnablePrefetch ? 1 : 0);
        }

        public void CommitImage()
        {
            Marshal.Copy(_image, 0, cuda_rv32i_ram_ptr(CoreId), _image.Length);
            InstallAccel();
            BuildJit(block: 1);   // single-instance latency path
            _committed = true;
        }

        // Translate the guest's RO code → CUDA, compile a per-guest JIT DLL, and
        // bind it. On any failure (no RO span, nvcc error) we leave _jit null and
        // StepN falls back to the interpreter for the whole run. The JIT shares
        // this DLL's managed CoreState[]/CoreMem[] (RvJitRuntime pulls the base
        // pointers via cuda_rv32i_state_ptr/mem_ptr), so reconcile keeps working.
        private void BuildJit(int block)
        {
            if (!UseJit) return;
            if (_codeHi <= _codeLo)
            {
                Console.Error.WriteLine("[cuda-jit] no read-only code span; using interpreter.");
                return;
            }
            try
            {
                byte[] code = _image[(int)_codeLo..(int)_codeHi];
                string buildDir = Path.Combine(AppContext.BaseDirectory, "jit");
                _jit = RvJitRuntime.Build(code, _codeLo, _codeHi, buildDir, block);
                Console.WriteLine($"[cuda-jit] active: {Path.GetFileName(_jit.DllPath)} " +
                                  $"({code.Length / 4} guest instrs, block={block}).");
            }
            catch (Exception ex)
            {
                _jit = null;
                Console.Error.WriteLine($"[cuda-jit] build failed, using interpreter:\n{ex.Message}");
            }
        }

        /// <summary>Broadcast the staged RAM image to ALL cores and set their sp/entry
        /// — used to run N independent copies of the same guest (throughput demo).</summary>
        public void CommitImageToAllCores(uint sp, uint entry)
        {
            for (int c = 0; c < NumCores; c++)
            {
                Marshal.Copy(_image, 0, cuda_rv32i_ram_ptr(c), _image.Length);
                cuda_rv32i_set_reg(c, 2, sp);
                cuda_rv32i_set_entry(c, entry);
            }
            InstallAccel();
            // Pack the JIT the same way the interpreter packs cores/block (1 core
            // per warp at NumCores==1; up to 256/block for the throughput demo).
            BuildJit(block: Math.Clamp(NumCores, 1, 256));
            _committed = true;
        }

        public void SetReg(int index, uint value) => cuda_rv32i_set_reg(CoreId, index, value);
        public void SetEntry(uint pc)             => cuda_rv32i_set_entry(CoreId, pc);

        /// <summary>Raw managed-buffer pointers (valid for host access between launches only).</summary>
        public IntPtr FramebufferPtr => cuda_rv32i_fb_ptr(CoreId);
        public IntPtr PcmPtr         => cuda_rv32i_pcm_ptr(CoreId);

        // ── Run ──────────────────────────────────────────────────────────────

        public int StepN(int n)
        {
            if (_halted) return 0;
            if (!_committed) throw new InvalidOperationException("CommitImage() must be called before StepN().");

            StageInputs();
            // The JIT kernel and the interpreter kernel are interchangeable per
            // launch: both run over the same managed CoreState[]/CoreMem[], so
            // peripheral reconcile (StageInputs/DrainOutputs) is identical.
            int rc = _jit != null ? _jit.StepAll(n) : cuda_rv32i_step_all(n);
            if (rc != 0)
            {
                string which = _jit != null ? "cuda_jit_step_all" : "cuda_rv32i_step_all";
                throw new InvalidOperationException($"{which} failed (CUDA error {rc})");
            }
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

            // Wall-clock time for the RTC + CLINT timer.
            ulong us  = (ulong)_clock.Elapsed.TotalMicroseconds;
            ulong ms  = (ulong)_clock.ElapsedMilliseconds;
            ulong ep  = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            uint  sec = (uint)_clock.Elapsed.TotalSeconds;
            cuda_rv32i_set_time(CoreId, (uint)us, (uint)(us >> 32), (uint)ms, (uint)(ms >> 32),
                                (uint)ep, (uint)(ep >> 32), sec, (uint)(us % 1_000_000UL));

            ulong ticks = (ulong)_clock.ElapsedTicks;
            ulong freq  = (ulong)Stopwatch.Frequency;
            ulong mtime = (ticks / freq) * TimebaseHz + (ticks % freq) * TimebaseHz / freq;
            cuda_rv32i_set_mtime(CoreId, (uint)mtime, (uint)(mtime >> 32));
            cuda_rv32i_set_mtip(CoreId, mtime >= cuda_rv32i_mtimecmp(CoreId) ? 1 : 0);
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
                int len = (int)cuda_rv32i_pcm_bytes(CoreId);
                System.Buffer.MemoryCopy((void*)PcmPtr, (void*)_audioMirror, len, len);
                AudioControl.WriteGeneration++;
            }

            // Framebuffer present: on vsync (or always, for guests that never
            // signal it) copy the managed FB → the host PresentedPixels SDL reads.
            uint vsync = cuda_rv32i_display_take_vsync(CoreId);
            uint fbAddr = cuda_rv32i_display_fbaddr(CoreId);
            int  fbLen = Math.Min((int)cuda_rv32i_fb_bytes(CoreId), Framebuffer.PresentedPixels.Length);
            IntPtr src = fbAddr != 0 && fbAddr < (uint)RamBytes ? cuda_rv32i_ram_ptr(CoreId) + (int)fbAddr
                                                                : FramebufferPtr;
            Marshal.Copy(src, Framebuffer.PresentedPixels, 0, fbLen);
        }

        public void Dispose()
        {
            if (_disposed) return;
            // Free the JIT DLL BEFORE the interpreter shuts down: the JIT holds
            // the managed pointers the interpreter owns, but never frees them.
            _jit?.Dispose(); _jit = null;
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
