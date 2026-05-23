using System.Runtime.InteropServices;
using System.Threading;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>MIDI output at 0x10005000. Guarded.</summary>
    public class MidiDevice : IPeripheral, IDisposable
    {
        public uint BaseAddress => 0x10005000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        // CRITICAL: this peripheral's Write runs inside the VEH callback,
        // which itself is nested below a P/Invoke (rv32i_step_n). On .NET 10
        // doing ANOTHER P/Invoke from inside VEH ("P/Invoke → VEH → P/Invoke")
        // trips the JIT's [UnmanagedCallersOnly] check and fast-fails the
        // process with "Invalid Program: attempted to call a UCO method
        // from managed code". So the VEH-side Write ONLY enqueues messages
        // into a lock-free ring; a background drain thread invokes the
        // winmm P/Invokes far away from any VEH frame.
        private IntPtr _midiHandle;
        private bool   _opened;
        private bool   _disposed;
        private Thread? _drainThread;

        // Ring of pending MIDI messages. Each slot holds either a 24-bit
        // ShortMsg (lower 24 bits, top byte 0) or a sentinel:
        //   0xFFFFFFFFu = "reset"
        //   0xFFFFFFFEu = "sleep N ms" — N stored in next slot
        private const int RingMask = (1 << 12) - 1;
        private readonly uint[] _ring = new uint[RingMask + 1];
        private long _head;   // producer (VEH thread)
        private long _tail;   // consumer (drain thread)

        [DllImport("winmm.dll")] private static extern int  midiOutOpen(out IntPtr lphMidiOut, uint uDeviceID, IntPtr dwCallback, IntPtr dwInstance, uint fdwOpen);
        [DllImport("winmm.dll")] private static extern int  midiOutShortMsg(IntPtr hMidiOut, uint dwMsg);
        [DllImport("winmm.dll")] private static extern int  midiOutReset(IntPtr hMidiOut);
        [DllImport("winmm.dll")] private static extern int  midiOutClose(IntPtr hMidiOut);
        [DllImport("winmm.dll")] private static extern uint midiOutGetNumDevs();

        public MidiDevice()
        {
            _drainThread = new Thread(DrainLoop)
            {
                IsBackground = true,
                Name         = "MidiDrain",
            };
            _drainThread.Start();
        }

        public uint Read(uint offset, int width)
        {
            if (offset != 0) return 0u;
            // Don't call P/Invoke from VEH — return a cached value.
            return _opened ? 1u : 0u;
        }

        public void Write(uint offset, int width, uint value)
        {
            // VEH-safe: only an interlocked ring-buffer enqueue. No P/Invoke,
            // no lock, no allocation.
            switch (offset)
            {
                case 0x04:                                    // short MIDI msg
                    if (value == 0) break;
                    Enqueue(value & 0x00FFFFFFu);
                    break;
                case 0x08:
                    if ((value & 1) != 0) Enqueue(0xFFFFFFFFu);   // reset
                    break;
                case 0x0C:
                    // Sleep request: enqueue sentinel + duration.
                    if (value > 0 && value <= 10_000)
                    {
                        Enqueue(0xFFFFFFFEu);
                        Enqueue(value);
                    }
                    break;
            }
        }

        private void Enqueue(uint v)
        {
            long slot = Interlocked.Increment(ref _head) - 1;
            _ring[(int)(slot & RingMask)] = v;
        }

        private void DrainLoop()
        {
            // Lazy open on first activity to avoid touching winmm at startup
            // when no MIDI device is installed.
            var spin = new SpinWait();
            while (!_disposed)
            {
                if (Volatile.Read(ref _head) == _tail)
                {
                    spin.SpinOnce();
                    continue;
                }
                spin.Reset();

                uint v = _ring[(int)(_tail & RingMask)];
                _tail++;

                try
                {
                    if (v == 0xFFFFFFFFu)
                    {
                        if (_opened) midiOutReset(_midiHandle);
                    }
                    else if (v == 0xFFFFFFFEu)
                    {
                        // Next slot is the sleep duration.
                        // Wait until producer has written it.
                        while (Volatile.Read(ref _head) == _tail) Thread.SpinWait(64);
                        uint ms = _ring[(int)(_tail & RingMask)];
                        _tail++;
                        if (ms > 0 && ms <= 10_000) Thread.Sleep((int)ms);
                    }
                    else
                    {
                        EnsureOpen();
                        if (_opened) midiOutShortMsg(_midiHandle, v);
                    }
                }
                catch { /* never let an exception escape into Windows */ }
            }
        }

        private void EnsureOpen()
        {
            if (_opened) return;
            if (midiOutGetNumDevs() == 0) return;
            if (midiOutOpen(out _midiHandle, 0, IntPtr.Zero, IntPtr.Zero, 0) != 0) return;
            _opened = true;
        }

        public void Dispose()
        {
            _disposed = true;
            if (_opened)
            {
                try { midiOutReset(_midiHandle); midiOutClose(_midiHandle); } catch { }
                _opened = false;
            }
        }
    }
}
