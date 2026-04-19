namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// Audio PCM buffer at 0x30000000 (1 MB).
    /// Audio control registers at 0x30100000.
    /// 
    /// Control register map (at 0x30100000):
    ///   +0x00 CTRL        (R/W) — bit 0=play, bit 1=loop, bit 2=stop
    ///   +0x04 STATUS      (RO)  — bit 0=playing, bit 1=buffer underrun
    ///   +0x08 SAMPLE_RATE (R/W) — default 22050
    ///   +0x0C CHANNELS    (R/W) — 1=mono, 2=stereo
    ///   +0x10 BIT_DEPTH   (R/W) — 8 or 16
    ///   +0x14 BUF_START   (R/W) — offset into audio buffer
    ///   +0x18 BUF_LENGTH  (R/W) — length in bytes
    ///   +0x1C POSITION    (RO)  — current playback position (offset from BUF_START)
    /// </summary>
    public class AudioBufferDevice : IPeripheral
    {
        public uint BaseAddress => 0x30000000;
        public uint Size => 0x100000; // 1 MB

        public byte[] Buffer { get; } = new byte[0x100000];

        public uint Read(uint offset, int width)
        {
            if (offset + (uint)width > Size) return 0;
            return width switch
            {
                1 => Buffer[offset],
                2 => (uint)(Buffer[offset] | (Buffer[offset + 1] << 8)),
                4 => (uint)(Buffer[offset] | (Buffer[offset + 1] << 8) |
                            (Buffer[offset + 2] << 16) | (Buffer[offset + 3] << 24)),
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            if (offset + (uint)width > Size) return;
            Buffer[offset] = (byte)value;
            if (width >= 2) Buffer[offset + 1] = (byte)(value >> 8);
            if (width >= 4)
            {
                Buffer[offset + 2] = (byte)(value >> 16);
                Buffer[offset + 3] = (byte)(value >> 24);
            }
        }
    }

    public class AudioControlDevice : IPeripheral
    {
        public uint BaseAddress => 0x30100000;
        public uint Size => 0x100;

        public uint Ctrl { get; set; }
        public uint SampleRate { get; set; } = 22050;
        public uint Channels { get; set; } = 1;
        public uint BitDepth { get; set; } = 16;
        public uint BufStart { get; set; }
        public uint BufLength { get; set; }
        public uint Position { get; set; }

        public bool IsPlaying => (Ctrl & 1) != 0;

        /// <summary>
        /// Incremented each time the guest signals new audio data is ready (CTRL bit 0 set).
        /// The SDL frontend uses this to detect new frames and avoid re-queuing stale data.
        /// </summary>
        public volatile uint WriteGeneration;

        public uint Read(uint offset, int width)
        {
            return offset switch
            {
                0x00 => Ctrl,
                0x04 => IsPlaying ? 1u : 0u,  // STATUS
                0x08 => SampleRate,
                0x0C => Channels,
                0x10 => BitDepth,
                0x14 => BufStart,
                0x18 => BufLength,
                0x1C => Position,
                _ => 0
            };
        }

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x00:
                    if ((value & 4) != 0) { Ctrl = 0; Position = 0; } // stop
                    else
                    {
                        if ((value & 1) != 0) WriteGeneration++; // new audio frame ready
                        Ctrl = value & 3;
                    }
                    break;
                case 0x08: SampleRate = value; break;
                case 0x0C: Channels = value; break;
                case 0x10: BitDepth = value; break;
                case 0x14: BufStart = value; break;
                case 0x18: BufLength = value; break;
            }
        }
    }
}
