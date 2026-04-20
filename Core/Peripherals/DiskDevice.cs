namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Block-read disk device at 0x10004000.
    /// Allows guest code to read chunks from a host file into guest RAM.
    ///
    /// Register map:
    ///   +0x00 OFFSET_LO (RW) — file offset low 32 bits
    ///   +0x04 OFFSET_HI (RW) — file offset high 32 bits (for files >4GB)
    ///   +0x08 LENGTH    (RW) — number of bytes to read
    ///   +0x0C DEST_ADDR (RW) — guest RAM address to copy into
    ///   +0x10 CMD       (RW) — write 1 to start read; host clears to 0 when done
    ///   +0x14 STATUS    (RO) — 0 = ok, 1 = error (e.g. out of bounds)
    ///   +0x18 FILE_SIZE_LO (RO) — total file size low 32 bits
    ///   +0x1C FILE_SIZE_HI (RO) — total file size high 32 bits
    /// </summary>
    public class DiskDevice : IPeripheral
    {
        public uint BaseAddress => 0x10004000;
        public uint Size => 0x100;

        private uint _offsetLo, _offsetHi;
        private uint _length;
        private uint _destAddr;
        private uint _cmd;
        private uint _status;

        private byte[]? _fileData;
        private MemoryBus? _bus;

        /// <summary>Load a file (or byte array) as the disk image.</summary>
        public void LoadFile(byte[] data, MemoryBus bus)
        {
            _fileData = data;
            _bus = bus;
        }

        public uint Read(uint offset, int width) => offset switch
        {
            0x00 => _offsetLo,
            0x04 => _offsetHi,
            0x08 => _length,
            0x0C => _destAddr,
            0x10 => _cmd,
            0x14 => _status,
            0x18 => (uint)(_fileData?.Length ?? 0),
            0x1C => (uint)((_fileData?.LongLength ?? 0) >> 32),
            _ => 0
        };

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x00: _offsetLo = value; break;
                case 0x04: _offsetHi = value; break;
                case 0x08: _length = value; break;
                case 0x0C: _destAddr = value; break;
                case 0x10:
                    _cmd = value;
                    if (value == 1) ExecuteRead();
                    break;
            }
        }

        private void ExecuteRead()
        {
            _status = 0;
            if (_fileData == null || _bus == null) { _status = 1; _cmd = 0; return; }

            long fileOffset = (long)_offsetLo | ((long)_offsetHi << 32);
            int len = (int)_length;

            if (fileOffset < 0 || fileOffset + len > _fileData.Length || len < 0)
            {
                _status = 1;
                _cmd = 0;
                return;
            }

            _bus.Load(_destAddr, _fileData, (int)fileOffset, len);
            _cmd = 0;
        }
    }
}
