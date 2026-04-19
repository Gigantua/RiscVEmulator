using System;
using System.Collections.Concurrent;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// 16550-compatible UART peripheral at 0x10000000.
    ///
    /// Register map (offsets from base, DLAB=0):
    ///   +0x00 RBR(R)/THR(W) — Receive Buffer / Transmit Holding
    ///   +0x01 IER           — Interrupt Enable Register (tracked so the 8250 driver's
    ///                          shadow register stays in sync with hardware reads)
    ///   +0x02 IIR(R)/FCR(W) — Interrupt ID (bit0=0 → "pending", drives polling timer)
    ///   +0x03 LCR           — Line Control (bit7=DLAB)
    ///   +0x05 LSR           — Line Status (bit0=DR, bit5=THRE, bit6=TEMT)
    ///   +0x06 MSR           — Modem Status (0 = no modem signals)
    ///
    /// When DLAB=1 offsets 0x00/0x01 access the baud-rate divisor latch (ignored).
    ///
    /// IIR always returns 0x00 (bit0=0 = "interrupt pending").  This keeps the
    /// 8250 polling timer active so it drains both the TX circular buffer and the
    /// RX FIFO on every poll tick.
    /// </summary>
    public class UartDevice : IPeripheral
    {
        public uint BaseAddress => 0x10000000;
        public uint Size        => 0x100;

        private readonly ConcurrentQueue<byte> _rxFifo = new();

        // Tracked registers
        private byte _ier    = 0;   // Interrupt Enable
        private byte _lcr    = 0;   // Line Control (bit7 = DLAB)
        private byte _divLo  = 0;   // Divisor latch low  (baud rate, accepted but ignored)
        private byte _divHi  = 0;   // Divisor latch high

        private bool Dlab => (_lcr & 0x80) != 0;

        /// <summary>Total THR writes (offset=0, non-DLAB) — includes kernel + user output.</summary>
        public long ThrWriteCount { get; private set; }

        /// <summary>Number of times IER was written with THRI bit set (bit 1 = TX interrupt enable).</summary>
        public long IerThriCount { get; private set; }

        /// <summary>Called when the guest writes a byte to the transmit register.</summary>
        public Action<char>? OutputHandler { get; set; }

        /// <summary>Enqueue a byte into the receive FIFO (called by host/test harness).</summary>
        public void EnqueueInput(byte b) => _rxFifo.Enqueue(b);

        public uint Read(uint offset, int width)
        {
            // DLAB=1: offsets 0/1 are divisor latch
            if (Dlab && offset == 0x00) return _divLo;
            if (Dlab && offset == 0x01) return _divHi;

            return offset switch
            {
                0x00 => _rxFifo.TryDequeue(out byte b) ? b : 0u,   // RBR
                0x01 => _ier,                                         // IER
                // IIR: always return 0x00 ("interrupt pending, modem status type").
                // serial8250_backup_timeout clears IER before reading IIR (when port.irq != 0),
                // so dynamic IIR based on _ier would see _ier=0 and return 0x01 (no pending),
                // preventing handle_irq from ever being called.  By returning 0x00 unconditionally
                // (matching mini-rv32ima), backup_timeout always calls serial8250_handle_irq, which
                // checks the driver's own up->ier shadow to decide whether to call serial8250_tx_chars.
                0x02 => 0x00u,
                // LSR: bit0=DR (RX data ready), bit5=THRE, bit6=TEMT (TX always empty)
                0x05 => (uint)((_rxFifo.IsEmpty ? 0 : 1) | 0x60),
                0x06 => 0x30u,   // MSR: CTS=1 (bit4) + DSR=1 (bit5)
                _    => 0u
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            // DLAB=1: offsets 0/1 are divisor latch (accept silently)
            if (Dlab && offset == 0x00) { _divLo = (byte)value; return; }
            if (Dlab && offset == 0x01) { _divHi = (byte)value; return; }

            switch (offset)
            {
                case 0x00:
                    ThrWriteCount++;
                    OutputHandler?.Invoke((char)(byte)value); break;  // THR
                case 0x01:
                    if (((byte)value & 0x02) != 0) IerThriCount++;
                    _ier = (byte)value; break;                         // IER
                case 0x03: _lcr = (byte)value; break;                         // LCR (updates DLAB)
                // FCR (0x02), MCR (0x04), SCR (0x07) — accepted, ignored
            }
        }
    }
}

