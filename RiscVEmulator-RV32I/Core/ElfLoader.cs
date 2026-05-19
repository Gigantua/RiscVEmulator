namespace RiscVEmulator.Core
{
    /// <summary>
    /// Minimal ELF32 loader for RISC-V bare-metal binaries.
    /// Reads PT_LOAD program headers and copies each segment into Memory at its virtual address.
    /// Returns the ELF entry point.
    /// </summary>
    public static class ElfLoader
    {
        private const uint ElfMagic  = 0x464C457F; // 0x7F 'E' 'L' 'F'
        private const uint PT_LOAD   = 1;

        public static uint Load(byte[] elf, IMemoryBus bus, uint physicalOffset = 0)
        {
            if (elf.Length < 52)
                throw new InvalidDataException("File too small to be a valid ELF32.");

            if (LE32(elf, 0) != ElfMagic)
                throw new InvalidDataException("Not a valid ELF file (bad magic).");

            if (elf[4] != 1)
                throw new InvalidDataException("Only ELF32 (EI_CLASS=1) is supported.");

            if (elf[5] != 1)
                throw new InvalidDataException("Only little-endian ELF (EI_DATA=1) is supported.");

            uint  entryPoint = LE32(elf, 24); // e_entry
            uint  phOff      = LE32(elf, 28); // e_phoff
            ushort phEntSize = LE16(elf, 42); // e_phentsize
            ushort phNum     = LE16(elf, 44); // e_phnum

            for (int i = 0; i < phNum; i++)
            {
                int   ph      = (int)phOff + i * phEntSize;
                uint  pType   = LE32(elf, ph +  0);
                uint  pOffset = LE32(elf, ph +  4);
                uint  pVAddr  = LE32(elf, ph +  8);
                uint  pFileSz = LE32(elf, ph + 16);
                uint  pMemSz  = LE32(elf, ph + 20);

                if (pType != PT_LOAD) continue;

                if (pFileSz > 0)
                    bus.Load(pVAddr - physicalOffset, elf, (int)pOffset, (int)pFileSz);

                // Zero BSS region (pMemSz > pFileSz)
                for (uint j = pFileSz; j < pMemSz; j++)
                    bus.WriteByte(pVAddr - physicalOffset + j, 0);
            }

            return entryPoint;
        }

        private static uint   LE32(byte[] b, int o) =>
            (uint)(b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24));

        private static ushort LE16(byte[] b, int o) =>
            (ushort)(b[o] | (b[o+1] << 8));
    }
}
