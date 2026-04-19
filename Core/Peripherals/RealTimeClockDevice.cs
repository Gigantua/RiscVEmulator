using System.Diagnostics;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Real-time wall clock at 0x10003000.
    /// Provides microsecond-resolution wall-clock time via host Stopwatch.
    /// Unlike the CLINT timer (which counts CPU steps), this tracks real elapsed time.
    ///
    /// Register map:
    ///   +0x00  TIME_LO    (RO) — microseconds since boot, low 32 bits
    ///   +0x04  TIME_HI    (RO) — microseconds since boot, high 32 bits
    ///   +0x08  TIME_MS_LO (RO) — milliseconds since boot, low 32 bits
    ///   +0x0C  TIME_MS_HI (RO) — milliseconds since boot, high 32 bits
    ///   +0x10  EPOCH_LO   (RO) — Unix epoch seconds, low 32 bits
    ///   +0x14  EPOCH_HI   (RO) — Unix epoch seconds, high 32 bits
    ///
    /// Guest reads TIME_LO/HI for high-resolution timing (Doom I_GetTime, etc.)
    /// Guest reads EPOCH_LO/HI for calendar time (gettimeofday, time()).
    /// </summary>
    public class RealTimeClockDevice : IPeripheral
    {
        public uint BaseAddress => 0x10003000;
        public uint Size => 0x100;

        private readonly Stopwatch _sw;

        public RealTimeClockDevice()
        {
            _sw = Stopwatch.StartNew();
        }

        public uint Read(uint offset, int width)
        {
            switch (offset)
            {
                case 0x00: // TIME_LO — microseconds since boot
                {
                    ulong us = (ulong)(_sw.Elapsed.TotalMicroseconds);
                    return (uint)us;
                }
                case 0x04: // TIME_HI
                {
                    ulong us = (ulong)(_sw.Elapsed.TotalMicroseconds);
                    return (uint)(us >> 32);
                }
                case 0x08: // TIME_MS_LO — milliseconds since boot
                {
                    ulong ms = (ulong)_sw.ElapsedMilliseconds;
                    return (uint)ms;
                }
                case 0x0C: // TIME_MS_HI
                {
                    ulong ms = (ulong)_sw.ElapsedMilliseconds;
                    return (uint)(ms >> 32);
                }
                case 0x10: // EPOCH_LO — Unix seconds
                {
                    ulong epoch = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
                    return (uint)epoch;
                }
                case 0x14: // EPOCH_HI
                {
                    ulong epoch = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
                    return (uint)(epoch >> 32);
                }
                case 0x18: // SECS_LO — seconds since boot (low 32 bits)
                {
                    ulong secs = (ulong)(_sw.Elapsed.TotalSeconds);
                    return (uint)secs;
                }
                case 0x1C: // USEC_FRAC — microseconds within current second (0–999999)
                {
                    var elapsed = _sw.Elapsed;
                    return (uint)(((ulong)elapsed.TotalMicroseconds) % 1000000UL);
                }
                default:
                    return 0;
            }
        }

        public void Write(uint offset, int width, uint value)
        {
            // All registers are read-only
        }
    }
}
