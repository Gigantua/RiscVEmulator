using System.IO.Compression;

namespace Examples.Linux;

/// <summary>
/// Minimal PNG encoder — writes an 8-bit RGBA framebuffer to a .png file.
/// Used by Rv64Boot's headless <c>--screenshot</c> path. Uses the BCL's
/// <see cref="ZLibStream"/> for IDAT compression; no external dependency.
/// </summary>
internal static class PngWriter
{
    public static void Write(string path, byte[] rgba, int width, int height)
    {
        // Raw image: each scanline is a filter byte (0 = None) + width*4 RGBA.
        var raw = new byte[height * (1 + width * 4)];
        int stride = width * 4;
        for (int y = 0; y < height; y++)
        {
            int src = y * stride;
            int dst = y * (1 + stride) + 1;          // +1 skips the filter byte
            Array.Copy(rgba, src, raw, dst, Math.Min(stride, rgba.Length - src));
        }

        using var fs = File.Create(path);
        fs.Write([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]); // PNG signature

        // IHDR — width, height, bit depth 8, colour type 6 (RGBA).
        var ihdr = new byte[13];
        WriteBE(ihdr, 0, (uint)width);
        WriteBE(ihdr, 4, (uint)height);
        ihdr[8] = 8;   // bit depth
        ihdr[9] = 6;   // colour type RGBA
        // ihdr[10..12] = compression/filter/interlace = 0
        WriteChunk(fs, "IHDR", ihdr);

        // IDAT — zlib-compressed raw image.
        using (var ms = new MemoryStream())
        {
            using (var z = new ZLibStream(ms, CompressionLevel.Optimal, leaveOpen: true))
                z.Write(raw, 0, raw.Length);
            WriteChunk(fs, "IDAT", ms.ToArray());
        }

        WriteChunk(fs, "IEND", []);
    }

    private static void WriteChunk(Stream s, string type, byte[] data)
    {
        Span<byte> len = stackalloc byte[4];
        WriteBE(len, 0, (uint)data.Length);
        s.Write(len);

        byte[] typeBytes = System.Text.Encoding.ASCII.GetBytes(type);
        s.Write(typeBytes);
        s.Write(data);

        uint crc = Crc32(typeBytes, data);
        Span<byte> crcb = stackalloc byte[4];
        WriteBE(crcb, 0, crc);
        s.Write(crcb);
    }

    private static void WriteBE(Span<byte> b, int o, uint v)
    {
        b[o] = (byte)(v >> 24); b[o+1] = (byte)(v >> 16);
        b[o+2] = (byte)(v >> 8); b[o+3] = (byte)v;
    }

    private static readonly uint[] CrcTable = BuildCrcTable();

    private static uint[] BuildCrcTable()
    {
        var t = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }

    private static uint Crc32(byte[] a, byte[] b)
    {
        uint c = 0xFFFFFFFFu;
        foreach (byte x in a) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        foreach (byte x in b) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
}
