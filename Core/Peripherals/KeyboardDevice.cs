using System.Collections.Concurrent;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Keyboard controller at 0x10001000.
    /// Register map:
    ///   +0x00 STATUS   (RO) — bit 0 = key event available
    ///   +0x04 DATA     (RO) — pop: bits [7:0]=scancode, bit 8=pressed(1)/released(0)
    ///   +0x08 MODIFIER (RO) — bit 0=shift, bit 1=ctrl, bit 2=alt
    /// </summary>
    public class KeyboardDevice : IPeripheral
    {
        public uint BaseAddress => 0x10001000;
        public uint Size => 0x100;

        private readonly ConcurrentQueue<uint> _fifo = new();
        private uint _modifiers;

        /// <summary>
        /// Enqueue a key event.
        /// </summary>
        /// <param name="scancode">Key scancode (0–255).</param>
        /// <param name="pressed">True for key-down, false for key-up.</param>
        public void EnqueueKey(byte scancode, bool pressed)
        {
            uint entry = (uint)(scancode | (pressed ? 0x100 : 0));
            _fifo.Enqueue(entry);
        }

        /// <summary>Set modifier state (called by host).</summary>
        public void SetModifiers(bool shift, bool ctrl, bool alt)
        {
            _modifiers = (shift ? 1u : 0) | (ctrl ? 2u : 0) | (alt ? 4u : 0);
        }

        public uint Read(uint offset, int width)
        {
            return offset switch
            {
                0x00 => _fifo.IsEmpty ? 0u : 1u,                        // STATUS
                0x04 => _fifo.TryDequeue(out uint val) ? val : 0u,      // DATA (pop)
                0x08 => _modifiers,                                      // MODIFIER
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            // Keyboard registers are read-only from the guest side
        }
    }
}
