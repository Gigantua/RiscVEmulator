using System.Runtime.InteropServices;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// Byte-addressable little-endian RAM. Backed by a PAGE_READWRITE slice
    /// inside a <see cref="HostMemoryReservation"/>. The native CPU reads/writes
    /// the same bytes via the reservation's base pointer — zero copy.
    /// </summary>
    public sealed unsafe class Memory
    {
        public HostMemoryReservation Reservation { get; }
        public uint BaseAddress { get; }
        public uint SizeBytes   { get; }
        public byte* RawPtr     => Reservation.PtrAt(BaseAddress);

        /// <summary>
        /// Creates its own reservation and commits RAM at <paramref name="baseAddress"/>.
        /// Default base is 0 (bare-metal). Linux uses 0x80000000.
        /// </summary>
        public Memory(int sizeInBytes, uint baseAddress = 0)
        {
            Reservation = new HostMemoryReservation();
            BaseAddress = baseAddress;
            SizeBytes   = (uint)sizeInBytes;
            Reservation.CommitPlain(baseAddress, SizeBytes);
        }

        /// <summary>Shared reservation (Linux uses this with baseAddress=0x80000000).</summary>
        public Memory(HostMemoryReservation reservation, uint baseAddress, uint sizeBytes)
        {
            Reservation = reservation;
            BaseAddress = baseAddress;
            SizeBytes   = sizeBytes;
            reservation.CommitPlain(baseAddress, sizeBytes);
        }

        public int SizeInBytes => (int)SizeBytes;

        // ── Reads ────────────────────────────────────────────────────────────

        public byte ReadByte(uint address)
        {
            CheckBounds(address, 1);
            return *(Reservation.PtrAt(address));
        }

        public ushort ReadHalfWord(uint address)
        {
            CheckBounds(address, 2);
            return *(ushort*)Reservation.PtrAt(address);
        }

        public uint ReadWord(uint address)
        {
            CheckBounds(address, 4);
            return *(uint*)Reservation.PtrAt(address);
        }

        // ── Writes ───────────────────────────────────────────────────────────

        public void WriteByte(uint address, byte value)
        {
            CheckBounds(address, 1);
            *(Reservation.PtrAt(address)) = value;
        }

        public void WriteHalfWord(uint address, ushort value)
        {
            CheckBounds(address, 2);
            *(ushort*)Reservation.PtrAt(address) = value;
        }

        public void WriteWord(uint address, uint value)
        {
            CheckBounds(address, 4);
            *(uint*)Reservation.PtrAt(address) = value;
        }

        // ── Bulk load (used by ELF loader) ───────────────────────────────────

        public void Load(uint address, byte[] src, int srcOffset, int length)
        {
            CheckBounds(address, (uint)length);
            fixed (byte* p = &src[srcOffset])
                Buffer.MemoryCopy(p, Reservation.PtrAt(address), length, length);
        }

        // ── Internal access (for compat) ─────────────────────────────────────
        // Returns a managed copy of the bytes [start, start+len). Slow path —
        // only used by code that legitimately needs a byte[] (e.g. DisplayControl
        // blitting from guest RAM into the presented framebuffer snapshot).
        internal byte[] Data
        {
            get
            {
                var arr = new byte[SizeBytes];
                Marshal.Copy(new IntPtr(Reservation.PtrAt(BaseAddress)), arr, 0, (int)SizeBytes);
                return arr;
            }
        }

        private void CheckBounds(uint address, uint size)
        {
            if (address < BaseAddress || (ulong)address + size > (ulong)BaseAddress + SizeBytes)
                throw new InvalidOperationException(
                    $"Memory access out of bounds: address=0x{address:X8} size={size} (RAM 0x{BaseAddress:X8}..0x{BaseAddress + SizeBytes:X8})");
        }
    }
}
