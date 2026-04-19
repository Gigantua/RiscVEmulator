namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Display control registers at 0x20100000.
    /// Register map:
    ///   +0x00 WIDTH      (RO)
    ///   +0x04 HEIGHT     (RO)
    ///   +0x08 BPP        (RO, always 32)
    ///   +0x0C VSYNC_FLAG (R/W — guest writes 1 to signal frame ready)
    ///   +0x10 PALETTE_INDEX (W — select palette entry 0–255)
    ///   +0x14 PALETTE_DATA  (W — write 0x00RRGGBB to selected entry)
    ///   +0x18 MODE       (R/W — 0=direct RGBA, 1=8-bit indexed)
    /// </summary>
    public class DisplayControlDevice : IPeripheral
    {
        public uint BaseAddress => 0x20100000;
        public uint Size => 0x100;

        private readonly FramebufferDevice _fb;
        private uint _vsyncFlag;
        private bool _vsyncEverUsed;
        private uint _paletteIndex;
        private uint _mode; // 0=direct RGBA, 1=indexed

        /// <summary>256-entry palette: each entry is 0x00RRGGBB.</summary>
        public uint[] Palette { get; } = new uint[256];

        public uint VsyncFlag => _vsyncFlag;
        public uint Mode => _mode;

        /// <summary>
        /// True once the guest has written vsync at least once.
        /// SDL uses this to decide whether to auto-present (Doom) or
        /// only show snapshots taken at vsync boundaries (Voxel).
        /// </summary>
        public bool VsyncEverUsed => _vsyncEverUsed;

        public DisplayControlDevice(FramebufferDevice fb)
        {
            _fb = fb;
        }

        /// <summary>Clear vsync flag (called by host after rendering a frame).</summary>
        public void ClearVsync() => _vsyncFlag = 0;

        public uint Read(uint offset, int width)
        {
            return offset switch
            {
                0x00 => (uint)_fb.Width,
                0x04 => (uint)_fb.Height,
                0x08 => FramebufferDevice.BytesPerPixel * 8,
                0x0C => _vsyncFlag,
                0x18 => _mode,
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x0C:
                    _vsyncFlag = value;
                    if (value != 0)
                    {
                        _vsyncEverUsed = true;
                        _fb.PresentFrame(); // snapshot live pixels → presented buffer
                    }
                    break;
                case 0x10: _paletteIndex = value & 0xFF; break;
                case 0x14: Palette[_paletteIndex] = value; break;
                case 0x18: _mode = value; break;
            }
        }
    }
}
