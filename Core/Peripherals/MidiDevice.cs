using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Memory-mapped MIDI output device at 0x10005000.
    /// Guest writes raw MIDI short messages; host plays them via Windows MIDI.
    ///
    /// Register map:
    ///   +0x00 STATUS (RO) — bit 0 = device ready
    ///   +0x04 DATA   (WO) — write a MIDI short message (status | data1&lt;&lt;8 | data2&lt;&lt;16)
    ///   +0x08 CTRL   (WO) — bit 0 = reset (all notes off)
    /// </summary>
    public class MidiDevice : IPeripheral, IDisposable
    {
        public uint BaseAddress => 0x10005000;
        public uint Size => 0x100;

        private IntPtr _midiHandle;
        private bool _opened;
        private readonly object _lock = new();

        // Win32 MIDI API (winmm.dll)
        [DllImport("winmm.dll")]
        private static extern int midiOutOpen(out IntPtr lphMidiOut, uint uDeviceID,
            IntPtr dwCallback, IntPtr dwInstance, uint fdwOpen);

        [DllImport("winmm.dll")]
        private static extern int midiOutShortMsg(IntPtr hMidiOut, uint dwMsg);

        [DllImport("winmm.dll")]
        private static extern int midiOutReset(IntPtr hMidiOut);

        [DllImport("winmm.dll")]
        private static extern int midiOutClose(IntPtr hMidiOut);

        [DllImport("winmm.dll")]
        private static extern uint midiOutGetNumDevs();

        private void EnsureOpen()
        {
            if (_opened) return;
            lock (_lock)
            {
                if (_opened) return;
                uint numDevs = midiOutGetNumDevs();
                if (numDevs == 0)
                {
                    Console.Error.WriteLine("MIDI: no output devices available");
                    return;
                }

                // Device 0 = Microsoft GS Wavetable Synth (default on Windows)
                int result = midiOutOpen(out _midiHandle, 0, IntPtr.Zero, IntPtr.Zero, 0);
                if (result != 0)
                {
                    Console.Error.WriteLine($"MIDI: midiOutOpen failed (error {result})");
                    return;
                }
                _opened = true;
            }
        }

        public uint Read(uint offset, int width)
        {
            return offset switch
            {
                0x00 => _opened ? 1u : (midiOutGetNumDevs() > 0 ? 1u : 0u), // STATUS
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x04: // DATA — send MIDI short message
                    if (value == 0) break;
                    EnsureOpen();
                    if (_opened)
                        midiOutShortMsg(_midiHandle, value);
                    break;

                case 0x08: // CTRL
                    if ((value & 1) != 0 && _opened)
                        midiOutReset(_midiHandle);
                    break;
            }
        }

        public void Dispose()
        {
            if (_opened)
            {
                midiOutReset(_midiHandle);
                midiOutClose(_midiHandle);
                _opened = false;
            }
        }
    }
}
