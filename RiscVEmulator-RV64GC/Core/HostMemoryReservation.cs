using System.Runtime.InteropServices;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// One big VA reservation covering the entire 4 GB guest address space.
    /// Reserved pages cost only a VAD entry — no physical memory or commit
    /// charge. The Emulator commits just the slices it needs (RAM, plain
    /// peripheral buffers as PAGE_READWRITE; guarded peripherals as
    /// PAGE_NOACCESS so accesses raise AVs the host VEH services).
    ///
    /// The native CPU dereferences <c>BasePtr + guest_addr</c> directly for
    /// every load/store — no plain-range table, no special cases.
    /// </summary>
    public sealed unsafe class HostMemoryReservation : IDisposable
    {
        public IntPtr Base { get; private set; }
        public ulong  Size { get; }

        private const uint MEM_RESERVE    = 0x00002000;
        private const uint MEM_COMMIT     = 0x00001000;
        private const uint MEM_RELEASE    = 0x00008000;
        private const uint PAGE_NOACCESS  = 0x01;
        private const uint PAGE_READWRITE = 0x04;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr VirtualAlloc(IntPtr lpAddress, ulong dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool VirtualFree(IntPtr lpAddress, ulong dwSize, uint dwFreeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool VirtualProtect(IntPtr lpAddress, ulong dwSize, uint flNewProtect, out uint lpflOldProtect);

        /// <param name="size">Total VA to reserve. Default 4 GB covers the entire RV32 address space.</param>
        public HostMemoryReservation(ulong size = 0x1_0000_0000UL)
        {
            Size = size;
            Base = VirtualAlloc(IntPtr.Zero, size, MEM_RESERVE, PAGE_NOACCESS);
            if (Base == IntPtr.Zero)
                throw new InvalidOperationException(
                    $"VirtualAlloc(RESERVE, {size}) failed (err {Marshal.GetLastWin32Error()})");
        }

        /// <summary>Commit a plain (PAGE_READWRITE) slice at <paramref name="guestOffset"/>.</summary>
        public IntPtr CommitPlain(uint guestOffset, uint sizeBytes)
            => CommitInternal(guestOffset, sizeBytes, PAGE_READWRITE);

        /// <summary>Commit a guarded (PAGE_NOACCESS) slice at <paramref name="guestOffset"/>.</summary>
        public IntPtr CommitGuarded(uint guestOffset, uint sizeBytes)
            => CommitInternal(guestOffset, sizeBytes, PAGE_NOACCESS);

        private IntPtr CommitInternal(uint guestOffset, uint sizeBytes, uint protect)
        {
            // Round up to page boundary.
            uint rounded = (sizeBytes + 0xFFFu) & ~0xFFFu;
            IntPtr addr  = Base + (nint)guestOffset;
            IntPtr got   = VirtualAlloc(addr, rounded, MEM_COMMIT, protect);
            if (got == IntPtr.Zero)
                throw new InvalidOperationException(
                    $"VirtualAlloc(COMMIT, 0x{guestOffset:X8}, {sizeBytes}, {protect:X}) failed (err {Marshal.GetLastWin32Error()})");
            return got;
        }

        public byte* PtrAt(uint guestOffset) => (byte*)Base + guestOffset;

        public Span<byte> SpanAt(uint guestOffset, int sizeBytes)
            => new(PtrAt(guestOffset), sizeBytes);

        public void Dispose()
        {
            if (Base != IntPtr.Zero)
            {
                VirtualFree(Base, 0, MEM_RELEASE);
                Base = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        ~HostMemoryReservation()
        {
            if (Base != IntPtr.Zero) VirtualFree(Base, 0, MEM_RELEASE);
        }
    }
}
