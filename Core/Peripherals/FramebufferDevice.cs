namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Framebuffer device at 0x20000000.
    /// Provides a direct-mapped RGBA8888 pixel buffer (default 320×200 = 256,000 bytes).
    /// The host reads <see cref="Pixels"/> to render.
    /// </summary>
    public class FramebufferDevice : IPeripheral
    {
        public uint BaseAddress => 0x20000000;
        public uint Size { get; }

        private readonly byte[] _pixels;
        private readonly byte[] _presented;

        public int Width { get; }
        public int Height { get; }
        public const int BytesPerPixel = 4; // RGBA8888

        /// <summary>Live pixel buffer — written by guest via MMIO.</summary>
        public byte[] Pixels => _pixels;

        /// <summary>
        /// Snapshot of the last completed frame, updated on vsync.
        /// SDL always blits from here so it never sees a partially-rendered frame.
        /// </summary>
        public byte[] PresentedPixels => _presented;

        public FramebufferDevice(int width = 320, int height = 200)
        {
            Width = width;
            Height = height;
            Size = (uint)(width * height * BytesPerPixel);
            _pixels    = new byte[Size];
            _presented = new byte[Size];
        }

        /// <summary>Snapshot live pixels → presented buffer. Called by DisplayControlDevice on vsync.</summary>
        public void PresentFrame() => Array.Copy(_pixels, _presented, _pixels.Length);

        public uint Read(uint offset, int width)
        {
            if (offset + (uint)width > Size) return 0;
            return width switch
            {
                1 => _pixels[offset],
                2 => (uint)(_pixels[offset] | (_pixels[offset + 1] << 8)),
                4 => (uint)(_pixels[offset] | (_pixels[offset + 1] << 8)
                     | (_pixels[offset + 2] << 16) | (_pixels[offset + 3] << 24)),
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            if (offset + (uint)width > Size) return;
            switch (width)
            {
                case 1:
                    _pixels[offset] = (byte)value;
                    break;
                case 2:
                    _pixels[offset]     = (byte)value;
                    _pixels[offset + 1] = (byte)(value >> 8);
                    break;
                case 4:
                    _pixels[offset]     = (byte)value;
                    _pixels[offset + 1] = (byte)(value >> 8);
                    _pixels[offset + 2] = (byte)(value >> 16);
                    _pixels[offset + 3] = (byte)(value >> 24);
                    break;
            }
        }
    }
}
