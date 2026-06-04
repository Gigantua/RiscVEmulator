using System.Diagnostics;
using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// CLINT (mtime / mtimecmp) memory-mapped timer. SiFive layout at
    /// 0x02000000 by default; Linux uses 0x11000000.
    ///
    /// This device owns the timer state. mtime is derived from a host
    /// monotonic clock so the guest sees ticks advance at TIMEBASE_HZ
    /// regardless of emulation speed. The CPU core holds no timer state — it
    /// only has an MTIP interrupt input pin, exactly like the MEIP pin the
    /// PLIC drives.
    ///
    /// <see cref="Tick"/> is polled by the <see cref="Emulator"/> between CPU
    /// step batches: it raises/lowers MTIP when mtime crosses mtimecmp.
    /// </summary>
    public sealed class ClintDevice : IPeripheral
    {
        // Guest timebase. Must match Examples/Linux DTB `timebase-frequency`.
        private const ulong TimebaseHz = 60_000_000UL;

        public uint BaseAddress { get; }
        public uint Size        => 0x10000;
        public bool IsGuarded   => true;

        public ClintDevice(uint baseAddress = 0x02000000u) { BaseAddress = baseAddress; }

        [DllImport("rv32i_core")] private static extern void rv32i_set_mtip(int level);

        private readonly Stopwatch _clock = Stopwatch.StartNew();
        private ulong _mtimecmp = ulong.MaxValue;

        /// <summary>Wall-clock-derived guest timer, in TIMEBASE_HZ ticks.</summary>
        public ulong Mtime
        {
            get
            {
                ulong d = (ulong)_clock.ElapsedTicks;
                ulong f = (ulong)Stopwatch.Frequency;
                // d * TimebaseHz / f, split to avoid 64-bit overflow.
                return (d / f) * TimebaseHz + (d % f) * TimebaseHz / f;
            }
        }

        /// <summary>
        /// Polled by the Emulator between CPU step batches: re-evaluates the
        /// machine timer interrupt pin (MTIP) as wall-clock time advances.
        /// </summary>
        public void Tick() => RefreshMtip();

        // MTIP is level-sensitive on (mtime >= mtimecmp), like a real CLINT
        // comparator: it must be re-evaluated whenever either input changes —
        // both as time advances (Tick) and the instant the guest rearms
        // mtimecmp (Write), or a just-rearmed timer interrupt re-fires in a
        // storm because the stale level is still asserted.
        private void RefreshMtip() => rv32i_set_mtip(Mtime >= _mtimecmp ? 1 : 0);

        public uint Read(uint offset, int width) => offset switch
        {
            0x0BFF8 => (uint) Mtime,
            0x0BFFC => (uint)(Mtime     >> 32),
            0x04000 => (uint) _mtimecmp,
            0x04004 => (uint)(_mtimecmp >> 32),
            _       => 0,
        };

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                // mtime is host-clock-derived; guest writes to it are advisory
                // and ignored — the epoch stays fixed.
                case 0x04000: _mtimecmp = (_mtimecmp & 0xFFFFFFFF00000000UL) |  value;               RefreshMtip(); break;
                case 0x04004: _mtimecmp = (_mtimecmp & 0x00000000FFFFFFFFUL) | ((ulong)value << 32);  RefreshMtip(); break;
            }
        }
    }
}
