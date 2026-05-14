namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Display control at 0x20100000. Guarded.</summary>
    public class DisplayControlDevice : IPeripheral
    {
        public uint BaseAddress => 0x20100000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private readonly FramebufferDevice _fb;
        private Memory? _ram;
        private uint _vsyncFlag;
        private bool _vsyncEverUsed;
        private uint _paletteIndex;
        private uint _mode;
        private uint _fbAddr;

        public uint[] Palette { get; } = new uint[256];

        public uint VsyncFlag     => _vsyncFlag;
        public uint Mode          => _mode;
        public bool VsyncEverUsed => _vsyncEverUsed;

        public DisplayControlDevice(FramebufferDevice fb) { _fb = fb; }
        public void SetMemory(Memory ram) => _ram = ram;
        public void ClearVsync() => _vsyncFlag = 0;

        public uint Read(uint offset, int width) => offset switch
        {
            0x00 => (uint)_fb.Width,
            0x04 => (uint)_fb.Height,
            0x08 => FramebufferDevice.BytesPerPixel * 8,
            0x0C => _vsyncFlag,
            0x18 => _mode,
            0x1C => _fbAddr,
            _    => 0,
        };

        public unsafe void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x0C:
                    _vsyncFlag = value;
                    if (value != 0)
                    {
                        _vsyncEverUsed = true;
                        if (_fbAddr != 0 && _ram != null)
                        {
                            int len = _fb.Width * _fb.Height * FramebufferDevice.BytesPerPixel;
                            uint addr = _fbAddr;
                            if (addr + len <= _ram.BaseAddress + _ram.SizeBytes)
                            {
                                fixed (byte* dst = _fb.PresentedPixels)
                                    System.Buffer.MemoryCopy(_ram.Reservation.PtrAt(addr), dst, len, len);
                            }
                        }
                        else
                        {
                            _fb.PresentFrame();
                        }
                    }
                    break;
                case 0x10: _paletteIndex = value & 0xFF; break;
                case 0x14: Palette[_paletteIndex] = value; break;
                case 0x18: _mode   = value; break;
                case 0x1C: _fbAddr = value; break;
            }
        }
    }
}
