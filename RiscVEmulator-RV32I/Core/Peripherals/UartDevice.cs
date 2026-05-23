using System;
using System.Collections.Concurrent;
using System.IO;
using System.Threading;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>16550-compatible UART at 0x10000000. Guarded.</summary>
    public class UartDevice : IPeripheral
    {
        public uint BaseAddress => 0x10000000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private readonly ConcurrentQueue<byte> _rxFifo = new();
        private byte _ier, _lcr, _mcr, _scr, _divLo, _divHi;
        private bool Dlab => (_lcr & 0x80) != 0;

        public long ThrWriteCount { get; private set; }
        public long IerThriCount  { get; private set; }

        // ── TX path ────────────────────────────────────────────────────────
        //
        // CRITICAL: the THR write happens inside the host VEH callback (an
        // x86 page-fault that Windows delivered on the CPU thread). Any
        // allocation or process-wide lock taken from that callback is a
        // hazard — Windows can interrupt the unwind, the .NET GC can try
        // to walk the JIT-emitted frame (which has no SEH metadata), and
        // the stack walker fast-fails the process with STATUS_STACK_-
        // BUFFER_OVERRUN (0xc0000409). Symptom: under a kernel oops
        // printk burst the host dies even though the kernel exception
        // handler is otherwise fine.
        //
        // Fix: the write side ONLY enqueues a byte (`ConcurrentQueue<byte>`
        // is lock-free, the boxed-int allocation per enqueue is small and
        // generational). A dedicated drain thread (DrainThread below)
        // pulls bytes off and runs the user's `Action<char>` on its own
        // stack — far away from VEH. The drain thread is started lazily
        // the first time someone subscribes to OutputHandler.
        // VEH-safe TX path. Single ring buffer of bytes. The write side
        // (THR write inside VEH) only does an interlocked tail-bump and a
        // byte store — NO allocation, NO managed lock. The drain side
        // reads bytes out and writes them as a raw byte[] to stdout via
        // a pre-allocated Stream (also no per-write allocation). Effect:
        // a kernel printk storm produces zero managed allocations on the
        // CPU thread, so GC never tries to walk the JIT frame.
        private const int TxRingMask = (1 << 16) - 1;
        private readonly byte[] _txRing = new byte[TxRingMask + 1];
        private long _txHead;   // producer (VEH thread) — Interlocked.Increment
        private long _txTail;   // consumer (drain thread)
        private Action<char>? _outputHandler;
        private Stream? _outputStream;
        private readonly byte[] _drainBuf = new byte[4096];
        private Thread? _drainThread;
        private readonly object _drainLock = new();

        public Action<char>? OutputHandler
        {
            get => _outputHandler;
            set
            {
                _outputHandler = value;
                if (value != null) EnsureDrainThread();
            }
        }

        /// <summary>
        /// Optional raw-byte sink. When set, the drain thread writes UART
        /// bytes directly to this stream — bypassing the per-char Action
        /// delegate (which is hard to make alloc-free). Call once at
        /// startup with <c>Console.OpenStandardOutput()</c> for the
        /// fastest, most VEH-safe path.
        /// </summary>
        public Stream? OutputStream
        {
            get => _outputStream;
            set
            {
                _outputStream = value;
                if (value != null) EnsureDrainThread();
            }
        }

        private void EnsureDrainThread()
        {
            if (_drainThread != null) return;
            lock (_drainLock)
            {
                if (_drainThread != null) return;
                _drainThread = new Thread(DrainLoop)
                {
                    IsBackground = true,
                    Name = "UartDrain",
                };
                _drainThread.Start();
            }
        }

        private void DrainLoop()
        {
            var spin = new SpinWait();
            while (true)
            {
                int n = 0;
                while (n < _drainBuf.Length)
                {
                    long head = Volatile.Read(ref _txHead);
                    if (head == _txTail) break;
                    _drainBuf[n++] = _txRing[(int)(_txTail & TxRingMask)];
                    _txTail++;
                }
                if (n == 0)
                {
                    spin.SpinOnce();
                    continue;
                }
                spin.Reset();
                // Raw-byte stream gets the FULL buffer in one Write — zero
                // alloc, no Console lock. Useful for the heavy-output path.
                var s = _outputStream;
                if (s != null)
                {
                    try { s.Write(_drainBuf, 0, n); }
                    catch { /* never crash on output */ }
                }
                // The per-char handler ALSO runs (so callers that want to
                // pattern-match on output for autostart / halt-token still
                // work). Both run on this drain thread — safe to allocate.
                var h = _outputHandler;
                if (h != null)
                {
                    for (int i = 0; i < n; i++)
                    {
                        try { h((char)_drainBuf[i]); } catch { }
                    }
                }
            }
        }

        public void EnqueueInput(byte b) => _rxFifo.Enqueue(b);

        public uint Read(uint offset, int width)
        {
            if (Dlab && offset == 0x00) return _divLo;
            if (Dlab && offset == 0x01) return _divHi;
            return offset switch
            {
                0x00 => _rxFifo.TryDequeue(out byte b) ? b : 0u,
                0x01 => _ier,
                // IIR: bits 6-7 = 0b11 → 16550A with FIFOs enabled (matches Linux 8250 driver
                // expectation when we report a working chip via scratch test).
                0x02 => 0xC0u,
                0x03 => _lcr,
                0x04 => _mcr,
                // LSR: bit 0 DR (data ready), bits 5-6 THRE+TEMT (TX empty — always true for us)
                0x05 => (uint)((_rxFifo.IsEmpty ? 0 : 1) | 0x60),
                0x06 => 0x30u,    // MSR: CTS+DSR asserted (typical)
                0x07 => _scr,
                _    => 0u,
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            if (Dlab && offset == 0x00) { _divLo = (byte)value; return; }
            if (Dlab && offset == 0x01) { _divHi = (byte)value; return; }
            switch (offset)
            {
                case 0x00:
                    ThrWriteCount++;
                    // VEH-safe: interlocked tail-bump + single byte store.
                    // No allocation, no managed lock — Windows VEH callback
                    // and the JIT-frame stack walk both stay clean.
                    long slot = Interlocked.Increment(ref _txHead) - 1;
                    _txRing[(int)(slot & TxRingMask)] = (byte)value;
                    break;
                case 0x01:
                    if (((byte)value & 0x02) != 0) IerThriCount++;
                    _ier = (byte)value;
                    break;
                case 0x02:
                    // FCR writes: ignored (we report FIFOs via IIR).
                    break;
                case 0x03:
                    _lcr = (byte)value;
                    break;
                case 0x04:
                    _mcr = (byte)value;
                    break;
                case 0x07:
                    _scr = (byte)value;     // scratch — used by the 8250 driver to detect chip
                    break;
            }
        }
    }
}
