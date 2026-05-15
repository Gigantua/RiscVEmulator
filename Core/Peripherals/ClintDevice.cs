using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// CLINT (mtime / mtimecmp) MMIO. Mapped at 0x02000000 by default
    /// (SiFive layout). Linux uses 0x11000000.
    ///
    /// mtime is the native CPU's instruction counter (increments once per
    /// step). Reads/writes are forwarded to the native CPU via P/Invoke.
    ///
    /// We rely on the DTS `timebase-frequency` being set to match the
    /// emulator's observed MIPS (see Examples.Linux — it patches the DTB
    /// at startup with a measured value). Without that the kernel thinks
    /// time runs many times faster than wall clock.
    /// </summary>
    public sealed class ClintDevice : IPeripheral
    {
        public uint BaseAddress { get; }
        public uint Size        => 0x10000;
        public bool IsGuarded   => true;

        public ClintDevice(uint baseAddress = 0x02000000u) { BaseAddress = baseAddress; }

        [DllImport("rv32i_core")] private static extern ulong rv32i_get_mtime();
        [DllImport("rv32i_core")] private static extern void  rv32i_set_mtime(ulong v);
        [DllImport("rv32i_core")] private static extern ulong rv32i_get_mtimecmp();
        [DllImport("rv32i_core")] private static extern void  rv32i_set_mtimecmp(ulong v);

        public uint Read(uint offset, int width) => offset switch
        {
            0x0BFF8 => (uint) rv32i_get_mtime(),
            0x0BFFC => (uint)(rv32i_get_mtime()    >> 32),
            0x04000 => (uint) rv32i_get_mtimecmp(),
            0x04004 => (uint)(rv32i_get_mtimecmp() >> 32),
            _       => 0,
        };

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x0BFF8: { ulong t = rv32i_get_mtime();    rv32i_set_mtime   ((t & 0xFFFFFFFF00000000UL) |  value);                  break; }
                case 0x0BFFC: { ulong t = rv32i_get_mtime();    rv32i_set_mtime   ((t & 0x00000000FFFFFFFFUL) | ((ulong)value << 32));    break; }
                case 0x04000: { ulong t = rv32i_get_mtimecmp(); rv32i_set_mtimecmp((t & 0xFFFFFFFF00000000UL) |  value);                  break; }
                case 0x04004: { ulong t = rv32i_get_mtimecmp(); rv32i_set_mtimecmp((t & 0x00000000FFFFFFFFUL) | ((ulong)value << 32));    break; }
            }
        }
    }
}
