using System;
using System.Collections.Concurrent;

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
        public Action<char>? OutputHandler { get; set; }

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
                    OutputHandler?.Invoke((char)(byte)value);
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
