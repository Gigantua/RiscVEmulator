namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Audio PCM buffer at 0x30000000 (1 MB). Plain memory.</summary>
    public sealed unsafe class AudioBufferDevice : IPeripheral
    {
        public uint BaseAddress => 0x30000000;
        public uint Size        => 0x100000;
        public bool IsGuarded   => false;

        private byte* _buf;

        /// <summary>Slice pointer in the host reservation. Null before <see cref="Bind"/>.</summary>
        public byte* RawPtr => _buf;
        public int   Length => (int)Size;

        /// <summary>Snapshot of audio bytes as a managed byte[] (slow — used by tests).</summary>
        public byte[] Buffer
        {
            get
            {
                var arr = new byte[Size];
                if (_buf != null)
                    System.Runtime.InteropServices.Marshal.Copy(new IntPtr(_buf), arr, 0, (int)Size);
                return arr;
            }
        }

        public void Bind(byte* slice) => _buf = slice;

        public uint Read(uint offset, int width)
        {
            if (_buf == null || offset + (uint)width > Size) return 0;
            byte* p = _buf + offset;
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
            if (_buf == null || offset + (uint)width > Size) return;
            byte* p = _buf + offset;
            p[0] = (byte)value;
            if (width >= 2) p[1] = (byte)(value >> 8);
            if (width >= 4)
            {
                p[2] = (byte)(value >> 16);
                p[3] = (byte)(value >> 24);
            }
        }
    }

    /// <summary>Audio control registers at 0x30100000. Guarded.</summary>
    public class AudioControlDevice : IPeripheral
    {
        public uint BaseAddress => 0x30100000;
        public uint Size        => 0x100;
        public bool IsGuarded   => true;

        public uint Ctrl       { get; set; }
        public uint SampleRate { get; set; } = 22050;
        public uint Channels   { get; set; } = 1;
        public uint BitDepth   { get; set; } = 16;
        public uint BufStart   { get; set; }
        public uint BufLength  { get; set; }
        public uint Position   { get; set; }

        public bool IsPlaying => (Ctrl & 1) != 0;
        public volatile uint WriteGeneration;

        public uint Read(uint offset, int width) => offset switch
        {
            0x00 => Ctrl,
            0x04 => IsPlaying ? 1u : 0u,
            0x08 => SampleRate,
            0x0C => Channels,
            0x10 => BitDepth,
            0x14 => BufStart,
            0x18 => BufLength,
            0x1C => Position,
            _    => 0,
        };

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x00:
                    if ((value & 4) != 0) { Ctrl = 0; Position = 0; }
                    else
                    {
                        if ((value & 1) != 0) WriteGeneration++;
                        Ctrl = value & 3;
                    }
                    break;
                case 0x08: SampleRate = value; break;
                case 0x0C: Channels   = value; break;
                case 0x10: BitDepth   = value; break;
                case 0x14: BufStart   = value; break;
                case 0x18: BufLength  = value; break;
            }
        }
    }
}
