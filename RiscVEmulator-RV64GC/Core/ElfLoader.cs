namespace RiscVEmulator.Core
{
    /// <summary>
    /// ELF64 loader for RISC-V (RV64GC) bare-metal binaries. Reads PT_LOAD
    /// program headers and copies each segment into <see cref="IMemoryBus"/>
    /// at its virtual address; returns the 64-bit entry point.
    ///
    /// The emulator is RV64-only — every guest is built <c>-mabi=lp64d</c> and
    /// links to a 64-bit ELF. ELF32 inputs are rejected outright. Guest
    /// physical addresses still fit inside the 4 GB host reservation, so the
    /// bus interface stays 32-bit; only the entry point is 64-bit.
    /// </summary>
    public static class ElfLoader
    {
        private const uint ElfMagic = 0x464C457F; // 0x7F 'E' 'L' 'F'
        private const uint PT_LOAD  = 1;

        public static ulong Load(byte[] elf, IMemoryBus bus, ulong physicalOffset = 0)
        {
            if (elf.Length < 64)
                throw new InvalidDataException("File too small to be a valid ELF64.");

            if (LE32(elf, 0) != ElfMagic)
                throw new InvalidDataException("Not a valid ELF file (bad magic).");

            if (elf[4] != 2)
                throw new InvalidDataException(
                    $"Only ELF64 (EI_CLASS=2) is supported; got EI_CLASS={elf[4]}. " +
                    "Rebuild the guest for RV64 (-mabi=lp64d).");

            if (elf[5] != 1)
                throw new InvalidDataException("Only little-endian ELF (EI_DATA=1) is supported.");

            ulong  entryPoint = LE64(elf, 24); // e_entry
            ulong  phOff      = LE64(elf, 32); // e_phoff
            ushort phEntSize  = LE16(elf, 54); // e_phentsize
            ushort phNum      = LE16(elf, 56); // e_phnum

            for (int i = 0; i < phNum; i++)
            {
                int  ph      = (int)phOff + i * phEntSize;
                uint pType   = LE32(elf, ph +  0);
                uint pOffset = (uint)LE64(elf, ph +  8);
                uint pVAddr  = (uint)LE64(elf, ph + 16);
                uint pFileSz = (uint)LE64(elf, ph + 32);
                uint pMemSz  = (uint)LE64(elf, ph + 40);

                if (pType != PT_LOAD) continue;

                uint dst = pVAddr - (uint)physicalOffset;

                if (pFileSz > 0)
                    bus.Load(dst, elf, (int)pOffset, (int)pFileSz);

                // Zero BSS region (pMemSz > pFileSz).
                for (uint j = pFileSz; j < pMemSz; j++)
                    bus.WriteByte(dst + j, 0);
            }

            return entryPoint;
        }

        private static uint LE32(byte[] b, int o) =>
            (uint)(b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24));

        private static ulong LE64(byte[] b, int o) =>
            LE32(b, o) | ((ulong)LE32(b, o + 4) << 32);

        private static ushort LE16(byte[] b, int o) =>
            (ushort)(b[o] | (b[o+1] << 8));
    }
}
