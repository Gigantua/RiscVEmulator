namespace RiscVEmulator.Core
{
    /// <summary>
    /// Byte-addressable little-endian memory for the RV32I emulator.
    /// </summary>
    public class Memory
    {
        private readonly byte[] _data;

        public int SizeInBytes => _data.Length;
        internal byte[] Data => _data;

        public Memory(int sizeInBytes)
        {
            _data = new byte[sizeInBytes];
        }

        // ── Reads ────────────────────────────────────────────────────────────

        public byte ReadByte(uint address)
        {
            CheckBounds(address, 1);
            return _data[address];
        }

        public ushort ReadHalfWord(uint address)
        {
            CheckBounds(address, 2);
            return (ushort)(_data[address] | (_data[address + 1] << 8));
        }

        public uint ReadWord(uint address)
        {
            CheckBounds(address, 4);
            return (uint)(_data[address]
                        | (_data[address + 1] << 8)
                        | (_data[address + 2] << 16)
                        | (_data[address + 3] << 24));
        }

        // ── Writes ───────────────────────────────────────────────────────────

        public void WriteByte(uint address, byte value)
        {
            CheckBounds(address, 1);
            _data[address] = value;
        }

        public void WriteHalfWord(uint address, ushort value)
        {
            CheckBounds(address, 2);
            _data[address]     = (byte)value;
            _data[address + 1] = (byte)(value >> 8);
        }

        public void WriteWord(uint address, uint value)
        {
            CheckBounds(address, 4);
            _data[address]     = (byte)value;
            _data[address + 1] = (byte)(value >> 8);
            _data[address + 2] = (byte)(value >> 16);
            _data[address + 3] = (byte)(value >> 24);
        }

        // ── Bulk load (used by ELF loader) ───────────────────────────────────

        public void Load(uint address, byte[] src, int srcOffset, int length)
        {
            CheckBounds(address, (uint)length);
            Array.Copy(src, srcOffset, _data, (int)address, length);
        }

        // ── Helpers ──────────────────────────────────────────────────────────

        private void CheckBounds(uint address, uint size)
        {
            if ((ulong)address + size > (ulong)_data.Length)
                throw new InvalidOperationException(
                    $"Memory access out of bounds: address=0x{address:X8} size={size} memSize=0x{_data.Length:X8}");
        }
    }
}
