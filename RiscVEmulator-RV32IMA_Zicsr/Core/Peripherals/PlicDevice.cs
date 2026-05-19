using System;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Platform-Level Interrupt Controller. SiFive-style layout at 0x0C000000.
    /// Single hart, two contexts: 0 = M-mode (MEIP), 1 = S-mode (SEIP).
    /// Devices raise IRQ N via <see cref="RaiseIrq"/>; the PLIC recomputes the
    /// highest pending+enabled IRQ per context and toggles the CPU's MEIP/SEIP
    /// bit. The guest claims/completes via reads/writes to the claim register.
    ///
    /// Memory layout (per the SiFive PLIC spec):
    ///   0x000000 + 4*N        priority[N]
    ///   0x001000              pending bit array  (read-only — guest can't write)
    ///   0x002000 + ctx*0x80   enable bit array   (32 words per context)
    ///   0x200000 + ctx*0x1000 threshold (offset 0), claim/complete (offset 4)
    /// </summary>
    public sealed class PlicDevice : IPeripheral
    {
        public const int MaxSources  = 32;       // 31 usable IRQs (source 0 reserved)
        public const int NumContexts = 2;        // M-mode + S-mode

        public uint BaseAddress { get; }
        public uint Size        => 0x0400_0000;  // standard PLIC window
        public bool IsGuarded   => true;

        private readonly uint[]   _priority = new uint[MaxSources];
        private uint              _pending;                                  // bit N = IRQ N pending
        private readonly uint[]   _enable    = new uint[NumContexts];        // bitmask per context
        private readonly uint[]   _threshold = new uint[NumContexts];
        private readonly uint[]   _claimed   = new uint[NumContexts];        // IRQ currently being serviced

        public PlicDevice(uint baseAddress = 0x0C00_0000) { BaseAddress = baseAddress; }

        // ── Device side ───────────────────────────────────────────────────────

        /// <summary>Edge-trigger: latch the IRQ as pending and re-evaluate.</summary>
        public void RaiseIrq(int source)
        {
            if (source <= 0 || source >= MaxSources) return;
            _pending |= 1u << source;
            UpdateCpuLines();
        }

        public void LowerIrq(int source)
        {
            if (source <= 0 || source >= MaxSources) return;
            _pending &= ~(1u << source);
            UpdateCpuLines();
        }

        // ── MMIO ──────────────────────────────────────────────────────────────

        public uint Read(uint offset, int width)
        {
            // Priority registers: 0x000000 + 4*N.
            if (offset < MaxSources * 4u)
                return _priority[offset >> 2];

            // Pending: 0x001000.
            if (offset >= 0x1000 && offset < 0x1000 + (MaxSources / 8))
                return _pending;

            // Enable arrays: 0x002000 + ctx*0x80. Each context gets 32 words.
            if (offset >= 0x2000 && offset < 0x2000 + NumContexts * 0x80u)
            {
                uint rel = offset - 0x2000u;
                int  ctx = (int)(rel / 0x80u);
                int  wd  = (int)((rel % 0x80u) / 4);
                return wd == 0 ? _enable[ctx] : 0;
            }

            // Threshold / claim per context: 0x200000 + ctx*0x1000.
            if (offset >= 0x20_0000 && offset < 0x20_0000 + NumContexts * 0x1000u)
            {
                uint rel = offset - 0x20_0000u;
                int  ctx = (int)(rel / 0x1000u);
                uint o   = rel % 0x1000u;
                if (o == 0) return _threshold[ctx];
                if (o == 4)
                {
                    // Claim: returns highest-priority pending+enabled IRQ above threshold,
                    // and atomically clears its pending bit. 0 means "nothing pending".
                    uint irq = HighestPendingEnabled(ctx);
                    if (irq != 0)
                    {
                        _pending &= ~(1u << (int)irq);
                        _claimed[ctx] = irq;
                        UpdateCpuLines();
                    }
                    return irq;
                }
            }
            return 0;
        }

        public void Write(uint offset, int width, uint value)
        {
            if (offset < MaxSources * 4u)
            {
                _priority[offset >> 2] = value & 7u;          // 3-bit priority per spec
                UpdateCpuLines();
                return;
            }

            if (offset >= 0x2000 && offset < 0x2000 + NumContexts * 0x80u)
            {
                uint rel = offset - 0x2000u;
                int  ctx = (int)(rel / 0x80u);
                int  wd  = (int)((rel % 0x80u) / 4);
                if (wd == 0)
                {
                    // IRQ 0 is reserved per spec; clear bit 0 of whatever the driver wrote.
                    _enable[ctx] = value & ~1u;
                    UpdateCpuLines();
                }
                return;
            }

            if (offset >= 0x20_0000 && offset < 0x20_0000 + NumContexts * 0x1000u)
            {
                uint rel = offset - 0x20_0000u;
                int  ctx = (int)(rel / 0x1000u);
                uint o   = rel % 0x1000u;
                if (o == 0) { _threshold[ctx] = value & 7u; UpdateCpuLines(); }
                else if (o == 4)
                {
                    // Complete: guest done servicing this IRQ. Allow re-fire if still asserted
                    // by the device (we model edge-triggered, so device must RaiseIrq again).
                    _claimed[ctx] = 0;
                    UpdateCpuLines();
                }
            }
        }

        // ── Pending/enabled evaluation ────────────────────────────────────────

        private uint HighestPendingEnabled(int ctx)
        {
            uint cand = _pending & _enable[ctx];
            if (cand == 0) return 0;
            uint best     = 0;
            uint bestPrio = 0;
            for (int i = 1; i < MaxSources; i++)
            {
                if ((cand & (1u << i)) == 0) continue;
                uint p = _priority[i];
                if (p > _threshold[ctx] && p > bestPrio)
                {
                    bestPrio = p;
                    best     = (uint)i;
                }
            }
            return best;
        }

        /// <summary>Toggle MEIP/SEIP based on whether each context has a deliverable IRQ.</summary>
        private void UpdateCpuLines()
        {
            Emulator.SetMachineExtIrq   (HighestPendingEnabled(0) != 0);
            Emulator.SetSupervisorExtIrq(HighestPendingEnabled(1) != 0);
        }
    }
}
