using System.Collections.Concurrent;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Keyboard at 0x10001000. Guarded — read pops the FIFO.</summary>
    public class KeyboardDevice : IPeripheral
    {
        public uint BaseAddress => 0x10001000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private readonly ConcurrentQueue<uint> _fifo = new();
        private uint _modifiers;

        public void EnqueueKey(byte scancode, bool pressed)
        {
            uint entry = (uint)(scancode | (pressed ? 0x100 : 0));
            _fifo.Enqueue(entry);
        }

        public void SetModifiers(bool shift, bool ctrl, bool alt)
            => _modifiers = (shift ? 1u : 0) | (ctrl ? 2u : 0) | (alt ? 4u : 0);

        public uint Read(uint offset, int width) => offset switch
        {
            0x00 => _fifo.IsEmpty ? 0u : 1u,
            0x04 => _fifo.TryDequeue(out uint val) ? val : 0u,
            0x08 => _modifiers,
            _    => 0u,
        };

        public void Write(uint offset, int width, uint value) { }
    }
}
