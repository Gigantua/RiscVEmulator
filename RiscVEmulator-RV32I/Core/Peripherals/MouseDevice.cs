namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Mouse at 0x10002000. Guarded — read clears deltas.</summary>
    public class MouseDevice : IPeripheral
    {
        public uint BaseAddress => 0x10002000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private int  _dx, _dy;
        private uint _buttons;
        private bool _hasData;

        public void MoveMouse(int dx, int dy) { _dx += dx; _dy += dy; _hasData = true; }

        public void SetButton(int button, bool pressed)
        {
            if (pressed) _buttons |= (1u << button);
            else         _buttons &= ~(1u << button);
            _hasData = true;
        }

        public uint Read(uint offset, int width)
        {
            switch (offset)
            {
                case 0x00: return _hasData ? 1u : 0u;
                case 0x04: { uint dx = (uint)_dx; _dx = 0; CheckClear(); return dx; }
                case 0x08: { uint dy = (uint)_dy; _dy = 0; CheckClear(); return dy; }
                case 0x0C: return _buttons;
                default:   return 0;
            }
        }

        public void Write(uint offset, int width, uint value) { }

        private void CheckClear() { if (_dx == 0 && _dy == 0) _hasData = _buttons != 0; }
    }
}
