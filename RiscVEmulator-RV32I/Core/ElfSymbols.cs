namespace RiscVEmulator.Core
{
    /// <summary>
    /// Minimal ELF32 symbol-table reader. Returns a name -> guest PC map for
    /// `STT_FUNC` symbols. Used (today) to register softfloat ABI entry
    /// points with the JIT so it can short-circuit them with inline x86 SSE.
    /// </summary>
    public static class ElfSymbols
    {
        public static Dictionary<string, uint> ReadFunctionSymbols(byte[] elf)
        {
            var result = new Dictionary<string, uint>(StringComparer.Ordinal);
            if (elf.Length < 52 || elf[4] != 1 /*ELF32*/ || elf[5] != 1 /*LE*/)
                return result;

            // ELF header section table offsets.
            uint shOff      = LE32(elf, 32);
            ushort shEntSize = LE16(elf, 46);
            ushort shNum     = LE16(elf, 48);
            ushort shStrIdx  = LE16(elf, 50);

            if (shNum == 0 || shOff == 0) return result;
            if (shOff + (uint)shNum * shEntSize > elf.Length) return result;

            // Locate the section-name string table (used to find ".symtab" /
            // ".strtab" by section name).
            int shStr = (int)shOff + shStrIdx * shEntSize;
            uint shStrTblOff = LE32(elf, shStr + 16);

            int symTabSec = -1, strTabSec = -1;
            for (int i = 0; i < shNum; i++)
            {
                int sh = (int)shOff + i * shEntSize;
                uint nameOff = LE32(elf, sh + 0);
                uint type    = LE32(elf, sh + 4);
                string name = ReadCString(elf, (int)(shStrTblOff + nameOff));
                if (type == 2 /*SHT_SYMTAB*/ && name == ".symtab") symTabSec = i;
                if (type == 3 /*SHT_STRTAB*/ && name == ".strtab") strTabSec = i;
            }
            if (symTabSec < 0 || strTabSec < 0) return result;

            int symSh = (int)shOff + symTabSec * shEntSize;
            uint symOff   = LE32(elf, symSh + 16);
            uint symSize  = LE32(elf, symSh + 20);
            uint symEntSz = LE32(elf, symSh + 36);

            int strSh = (int)shOff + strTabSec * shEntSize;
            uint strOff = LE32(elf, strSh + 16);

            if (symEntSz == 0) return result;
            uint symCount = symSize / symEntSz;
            for (uint i = 1; i < symCount; i++)
            {
                int sym  = (int)(symOff + i * symEntSz);
                uint sNameOff = LE32(elf, sym + 0);
                uint sValue   = LE32(elf, sym + 4);
                byte sInfo    = elf[sym + 12];
                int  sType    = sInfo & 0xF;          // STT_*
                if (sType != 2 /*STT_FUNC*/) continue;
                if (sValue == 0) continue;
                string sName = ReadCString(elf, (int)(strOff + sNameOff));
                if (sName.Length == 0) continue;
                result[sName] = sValue;
            }
            return result;
        }

        private static string ReadCString(byte[] b, int o)
        {
            if (o < 0 || o >= b.Length) return string.Empty;
            int end = o;
            while (end < b.Length && b[end] != 0) end++;
            return System.Text.Encoding.ASCII.GetString(b, o, end - o);
        }

        private static uint   LE32(byte[] b, int o) =>
            (uint)(b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24));
        private static ushort LE16(byte[] b, int o) =>
            (ushort)(b[o] | (b[o+1] << 8));
    }
}
