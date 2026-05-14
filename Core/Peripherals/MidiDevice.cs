using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>MIDI output at 0x10005000. Guarded.</summary>
    public class MidiDevice : IPeripheral, IDisposable
    {
        public uint BaseAddress => 0x10005000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        private IntPtr _midiHandle;
        private bool _opened;
        private readonly object _lock = new();

        [DllImport("winmm.dll")] private static extern int midiOutOpen(out IntPtr lphMidiOut, uint uDeviceID, IntPtr dwCallback, IntPtr dwInstance, uint fdwOpen);
        [DllImport("winmm.dll")] private static extern int midiOutShortMsg(IntPtr hMidiOut, uint dwMsg);
        [DllImport("winmm.dll")] private static extern int midiOutReset(IntPtr hMidiOut);
        [DllImport("winmm.dll")] private static extern int midiOutClose(IntPtr hMidiOut);
        [DllImport("winmm.dll")] private static extern uint midiOutGetNumDevs();

        private void EnsureOpen()
        {
            if (_opened) return;
            lock (_lock)
            {
                if (_opened) return;
                if (midiOutGetNumDevs() == 0) return;
                if (midiOutOpen(out _midiHandle, 0, IntPtr.Zero, IntPtr.Zero, 0) != 0) return;
                _opened = true;
            }
        }

        public uint Read(uint offset, int width) => offset == 0
            ? (_opened ? 1u : (midiOutGetNumDevs() > 0 ? 1u : 0u))
            : 0u;

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x04:
                    if (value == 0) break;
                    EnsureOpen();
                    if (_opened) midiOutShortMsg(_midiHandle, value);
                    break;
                case 0x08:
                    if ((value & 1) != 0 && _opened) midiOutReset(_midiHandle);
                    break;
                case 0x0C:
                    if (value > 0 && value <= 10_000) Thread.Sleep((int)value);
                    break;
            }
        }

        public void Dispose()
        {
            if (_opened) { midiOutReset(_midiHandle); midiOutClose(_midiHandle); _opened = false; }
        }
    }
}
