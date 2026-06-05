using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Framebuffer at <see cref="BaseAddress"/> (default 0x20000000). Plain
    /// memory (PAGE_READWRITE slice). CPU writes pixels directly via mem+addr
    /// — no callback. SDL reads the tear-free <see cref="PresentedPixels"/>
    /// snapshot.
    ///
    /// The Linux example overrides BaseAddress to live INSIDE RAM (top 256 KB
    /// of /memory) so the kernel can use the standard simple-framebuffer
    /// driver without tripping the init_unavailable_range trap — see
    /// CLAUDE.md "Don't put simple-framebuffer in the DT" for the gory
    /// details.
    /// </summary>
    public sealed unsafe class FramebufferDevice : IPeripheral
    {
        public uint BaseAddress { get; }
        public uint Size { get; }
        public bool IsGuarded => false;

        public int Width { get; }
        public int Height { get; }
        public const int BytesPerPixel = 4;

        private byte*  _pixels;       // slice in the host reservation
        private readonly byte[] _presented;

        /// <summary>Live pixel buffer pointer (slice in reservation). Null until <see cref="Bind"/>.</summary>
        public byte* PixelsPtr => _pixels;

        /// <summary>
        /// Snapshot of live pixels as a managed byte[] (slow — used by tests).
        /// Production code should read <see cref="PixelsPtr"/> directly.
        /// </summary>
        public byte[] Pixels
        {
            get
            {
                var arr = new byte[Size];
                if (_pixels != null)
                    System.Runtime.InteropServices.Marshal.Copy(new IntPtr(_pixels), arr, 0, (int)Size);
                return arr;
            }
        }

        /// <summary>Tear-free snapshot, updated on vsync. SDL reads from this.</summary>
        public byte[] PresentedPixels => _presented;

        public FramebufferDevice(int width = 320, int height = 200, uint baseAddress = 0x20000000)
        {
            BaseAddress = baseAddress;
            Width  = width;
            Height = height;
            Size   = (uint)(width * height * BytesPerPixel);
            _presented = new byte[Size];
        }

        public void Bind(byte* slice) => _pixels = slice;

        public void PresentFrame()
        {
            if (_pixels == null) return;
            fixed (byte* dst = _presented)
                Buffer.MemoryCopy(_pixels, dst, _presented.Length, _presented.Length);
        }

        // Read/Write are unused for plain peripherals (CPU dereferences directly),
        // but kept for in-process callers (tests, frontends).
        public uint Read(uint offset, int width)
        {
            if (_pixels == null || offset + (uint)width > Size) return 0;
            byte* p = _pixels + offset;
            return width switch
            {
                1 => p[0],
                2 => (uint)(p[0] | (p[1] << 8)),
                4 => (uint)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)),
                _ => 0,
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            if (_pixels == null || offset + (uint)width > Size) return;
            byte* p = _pixels + offset;
            p[0] = (byte)value;
            if (width >= 2) p[1] = (byte)(value >> 8);
            if (width >= 4)
            {
                p[2] = (byte)(value >> 16);
                p[3] = (byte)(value >> 24);
            }
        }
    }
}
