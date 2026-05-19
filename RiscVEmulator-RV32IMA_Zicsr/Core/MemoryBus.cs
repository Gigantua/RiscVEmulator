using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// Address-dispatching memory bus. Routes accesses to RAM or registered peripherals.
    /// Optimized: RAM accesses short-circuit without scanning peripherals.
    /// </summary>
    public class MemoryBus : IMemoryBus
    {
        private readonly Memory _ram;
        private readonly List<IPeripheral> _peripherals = new();
        private readonly uint _ramEnd;

        // Cache last-hit peripheral to speed up sequential MMIO access patterns
        private IPeripheral? _lastHit;
        private uint _lastBase;
        private uint _lastEnd;

        public int RamSize => _ram.SizeInBytes;
        internal Memory RawMemory => _ram;
        public IReadOnlyList<IPeripheral> Peripherals => _peripherals;

        public MemoryBus(Memory ram)
        {
            _ram = ram;
            _ramEnd = (uint)ram.SizeInBytes;
        }

        public void RegisterPeripheral(IPeripheral peripheral)
        {
            _peripherals.Add(peripheral);
        }

        // ── Reads ────────────────────────────────────────────────────────────

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public byte ReadByte(uint address)
        {
            if (address < _ramEnd)
                return _ram.ReadByte(address);
            if (TryGetPeripheral(address, out var p, out uint offset))
                return (byte)p.Read(offset, 1);
            return 0;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public ushort ReadHalfWord(uint address)
        {
            if (address < _ramEnd)
                return _ram.ReadHalfWord(address);
            if (TryGetPeripheral(address, out var p, out uint offset))
                return (ushort)p.Read(offset, 2);
            return 0;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public uint ReadWord(uint address)
        {
            if (address < _ramEnd)
                return _ram.ReadWord(address);
            if (TryGetPeripheral(address, out var p, out uint offset))
                return p.Read(offset, 4);
            return 0;
        }

        // ── Writes ───────────────────────────────────────────────────────────

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void WriteByte(uint address, byte value)
        {
            if (address < _ramEnd)
            { _ram.WriteByte(address, value); return; }
            if (TryGetPeripheral(address, out var p, out uint offset))
                p.Write(offset, 1, value);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void WriteHalfWord(uint address, ushort value)
        {
            if (address < _ramEnd)
            { _ram.WriteHalfWord(address, value); return; }
            if (TryGetPeripheral(address, out var p, out uint offset))
                p.Write(offset, 2, value);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void WriteWord(uint address, uint value)
        {
            if (address < _ramEnd)
            { _ram.WriteWord(address, value); return; }
            if (TryGetPeripheral(address, out var p, out uint offset))
                p.Write(offset, 4, value);
        }

        // ── Bulk load (RAM only) ─────────────────────────────────────────────

        public void Load(uint address, byte[] src, int srcOffset, int length)
        {
            _ram.Load(address, src, srcOffset, length);
        }

        // ── Peripheral lookup with last-hit cache ────────────────────────────

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private bool TryGetPeripheral(uint address, out IPeripheral peripheral, out uint offset)
        {
            // Check last-hit cache first (covers sequential framebuffer/audio writes)
            if (_lastHit != null && address >= _lastBase && address < _lastEnd)
            {
                peripheral = _lastHit;
                offset = address - _lastBase;
                return true;
            }

            for (int i = 0; i < _peripherals.Count; i++)
            {
                var p = _peripherals[i];
                uint pBase = p.BaseAddress;
                uint pEnd = pBase + p.Size;
                if (address >= pBase && address < pEnd)
                {
                    _lastHit = p;
                    _lastBase = pBase;
                    _lastEnd = pEnd;
                    peripheral = p;
                    offset = address - pBase;
                    return true;
                }
            }
            peripheral = null!;
            offset = 0;
            return false;
        }
    }
}
