namespace RiscVEmulator.Core
{
    /// <summary>
    /// Abstraction over the CPU's memory interface. All loads and stores go through this.
    /// Implementations route addresses to RAM, MMIO peripherals, or raise bus faults.
    /// </summary>
    public interface IMemoryBus
    {
        byte   ReadByte(uint address);
        ushort ReadHalfWord(uint address);
        uint   ReadWord(uint address);

        void WriteByte(uint address, byte value);
        void WriteHalfWord(uint address, ushort value);
        void WriteWord(uint address, uint value);

        /// <summary>Bulk load into RAM (used by ELF loader). Peripherals need not support this.</summary>
        void Load(uint address, byte[] src, int srcOffset, int length);

        int RamSize { get; }
    }
}
