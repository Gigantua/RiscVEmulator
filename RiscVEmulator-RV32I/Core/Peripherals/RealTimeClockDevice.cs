using System.Diagnostics;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Real-time wall clock at 0x10003000. Guarded.</summary>
    public sealed class RealTimeClockDevice : IPeripheral
    {
        public uint BaseAddress => 0x10003000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private readonly Stopwatch _sw = Stopwatch.StartNew();

        public uint Read(uint offset, int width)
        {
            switch (offset)
            {
                case 0x00: { ulong us = (ulong)_sw.Elapsed.TotalMicroseconds; return (uint)us; }
                case 0x04: { ulong us = (ulong)_sw.Elapsed.TotalMicroseconds; return (uint)(us >> 32); }
                case 0x08: { ulong ms = (ulong)_sw.ElapsedMilliseconds;       return (uint)ms; }
                case 0x0C: { ulong ms = (ulong)_sw.ElapsedMilliseconds;       return (uint)(ms >> 32); }
                case 0x10: return (uint) DateTimeOffset.UtcNow.ToUnixTimeSeconds();
                case 0x14: return (uint)(DateTimeOffset.UtcNow.ToUnixTimeSeconds() >> 32);
                case 0x18: return (uint)(ulong)_sw.Elapsed.TotalSeconds;
                case 0x1C: return (uint)(((ulong)_sw.Elapsed.TotalMicroseconds) % 1000000UL);
                default:   return 0;
            }
        }

        public void Write(uint offset, int width, uint value) { }
    }
}
