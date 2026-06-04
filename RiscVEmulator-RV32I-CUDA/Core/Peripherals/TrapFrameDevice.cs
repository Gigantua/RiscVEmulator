namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// The hardware trap-frame page. Plain RAM (PAGE_READWRITE) — the CPU and
    /// the guest both read and write it directly, no callback.
    ///
    /// This page replaces the old paravirt CSR MMIO device: there are no CSR
    /// accessors any more. On a trap the native CPU spills the integer
    /// register file and the trap cause into the landing pad here, then jumps
    /// to the handler at <c>TRAP_VECTOR</c>; on return it reloads the file
    /// from a handler-supplied frame. Peripherals only ever *raise an
    /// interrupt pin* (see <see cref="Emulator.SetMachineExtIrq"/> and the
    /// CLINT's MTIP) — the CPU does the context switch itself.
    ///
    /// Layout (see Native/rv32i_core.cpp for the authoritative copy):
    ///   +0x000  IE_FLAG      interrupt enable, mstatus image (bit3 MIE = on)
    ///   +0x004  TRAP_VECTOR  handler entry PC (guest writes once at boot)
    ///   +0x008  IE_MASK      per-source enable (bit7 = MTIP, bit11 = MEIP)
    ///   +0x00C  TRAP_SCRATCH mscratch-equivalent (trap entry swaps with tp)
    ///   +0x100  landing pad  36-word trap frame, RISC-V struct pt_regs
    ///                        layout: word[0]=epc, word[1..31]=x1..x31,
    ///                        word[32]=status, word[33]=tval, word[34]=cause
    ///
    /// The page is committed zero-filled, so at boot interrupts are disabled
    /// and no handler is installed until the guest writes one.
    /// </summary>
    public sealed class TrapFrameDevice : IPeripheral
    {
        public uint BaseAddress { get; }
        public uint Size => 0x1000;
        public bool IsGuarded => false;   // plain RAM: CPU/guest access directly

        public TrapFrameDevice(uint baseAddress = 0x0F00_0000u)
        {
            BaseAddress = baseAddress;
        }

        // Plain peripheral: the CPU dereferences the committed slice directly,
        // so Read/Write are never invoked and Bind has nothing to stash.
        public uint Read(uint offset, int width) => 0;
        public void Write(uint offset, int width, uint value) { }
    }
}
