using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

/// <summary>
/// Exercise every compiler-rt integer libcall in Runtime/runtime.c against
/// edge-case operands. Guest output is compared byte-for-byte to a host
/// reference produced by the same C# code.
///
/// On divergence, the assertion reports the first failing line with its
/// op label, indices, and expected vs actual hex — so a single failure
/// names the broken libcall.
/// </summary>
[TestClass]
public class IntLibcallsTest : EmulatorTestBase
{
    static readonly uint[] U32 =
    {
        0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFu, 0xFFFFu,
        0x10000u, 0x12345678u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
        0xFFFFFFFEu, 0xFFFFFFFFu, 0xDEADBEEFu,
    };
    static readonly int[] S32 =
    {
        0, 1, -1, 2, -2, 127, -128, 0x7FFF,
        -0x7FFF - 1, 0x12345, -0x12345, 0x7FFFFFFF,
        unchecked((int)0x80000000), unchecked((int)0x80000001), -0x10000, 0x10000,
    };
    static readonly ulong[] U64 =
    {
        0UL, 1UL, 0xFFFFFFFFUL, 0x100000000UL,
        0x123456789ABCDEFUL, 0x8000000000000000UL,
        0xFFFFFFFFFFFFFFFFUL, 0xDEADBEEFCAFEBABEUL,
    };
    static readonly long[] S64 =
    {
        0L, 1L, -1L, 0x7FFFFFFFFFFFFFFFL,
        unchecked((long)0x8000000000000000L), 12345678901234L,
        -12345678901234L, unchecked((long)0xCAFEBABEDEADBEEFL),
    };

    static string Expected()
    {
        var sb = new StringBuilder(1 << 20);
        sb.Append("MUL32\n");
        foreach (var a in U32) foreach (var b in U32)
            sb.AppendFormat("{0:x8}\n", unchecked(a * b));
        sb.Append("UDIV32\n");
        foreach (var a in U32) foreach (var b in U32)
        {
            sb.AppendFormat("{0:x8}\n", b != 0 ? a / b : 0xFFFFFFFFu);
            sb.AppendFormat("{0:x8}\n", b != 0 ? a % b : a);
        }
        sb.Append("SDIV32\n");
        foreach (var a in S32) foreach (var b in S32)
        {
            int q, r;
            if (b == 0) { q = -1; r = a; }
            else if (a == unchecked((int)0x80000000) && b == -1) { q = a; r = 0; }
            else { q = a / b; r = a % b; }
            sb.AppendFormat("{0:x8}\n", unchecked((uint)q));
            sb.AppendFormat("{0:x8}\n", unchecked((uint)r));
        }
        sb.Append("MUL64\n");
        foreach (var a in U64) foreach (var b in U64)
            sb.AppendFormat("{0:x16}\n", unchecked(a * b));
        sb.Append("UDIV64\n");
        foreach (var a in U64) foreach (var b in U64)
        {
            sb.AppendFormat("{0:x16}\n", b != 0 ? a / b : 0xFFFFFFFFFFFFFFFFUL);
            sb.AppendFormat("{0:x16}\n", b != 0 ? a % b : a);
        }
        sb.Append("SDIV64\n");
        foreach (var a in S64) foreach (var b in S64)
        {
            long q, r;
            if (b == 0) { q = -1; r = a; }
            else if (a == unchecked((long)0x8000000000000000L) && b == -1) { q = a; r = 0; }
            else { q = a / b; r = a % b; }
            sb.AppendFormat("{0:x16}\n", unchecked((ulong)q));
            sb.AppendFormat("{0:x16}\n", unchecked((ulong)r));
        }
        sb.Append("SHL64\n");
        foreach (var a in U64) for (int s = 0; s < 64; s++)
            sb.AppendFormat("{0:x16}\n", unchecked(a << s));
        sb.Append("SHR64\n");
        foreach (var a in U64) for (int s = 0; s < 64; s++)
            sb.AppendFormat("{0:x16}\n", a >> s);
        sb.Append("SAR64\n");
        foreach (var a in S64) for (int s = 0; s < 64; s++)
            sb.AppendFormat("{0:x16}\n", unchecked((ulong)(a >> s)));
        sb.Append("DONE\n");
        return sb.ToString();
    }

    [TestMethod]
    public void RuntimeIntegerLibcallsMatchHost()
    {
        string elf = Path.Combine(TestDir, "int_libcalls.elf");
        string src = Path.Combine(ProgramDir, "int_libcalls.c");
        string rtObj = EnsureRuntimeObject();
        CompileC(new[] { src, rtObj }, elf);
        var (output, _, _) = RunElf(elf, maxSteps: 200_000_000);

        string expected = Expected();
        if (output == expected) return;

        // Find first divergence and report context.
        var exLines  = expected.Split('\n');
        var gotLines = output  .Split('\n');
        int max = Math.Min(exLines.Length, gotLines.Length);
        int diff = -1;
        for (int i = 0; i < max; i++) if (exLines[i] != gotLines[i]) { diff = i; break; }
        if (diff < 0) diff = max;

        // Walk back to find current op label.
        string label = "?";
        for (int i = diff; i >= 0; i--)
            if (exLines[i].Length > 0 && !IsHex(exLines[i])) { label = exLines[i]; break; }

        var msg = new StringBuilder();
        msg.AppendLine($"Divergence in section [{label}] at line {diff}:");
        for (int i = Math.Max(0, diff - 3); i <= Math.Min(max - 1, diff + 1); i++)
        {
            string ex = i < exLines.Length  ? exLines[i]  : "<eof>";
            string gt = i < gotLines.Length ? gotLines[i] : "<eof>";
            string mark = i == diff ? " <-- HERE" : "";
            msg.AppendLine($"  line {i}: expected={ex,-18} got={gt,-18}{mark}");
        }
        msg.AppendLine($"expected total lines={exLines.Length}, got total lines={gotLines.Length}");
        Assert.Fail(msg.ToString());

        static bool IsHex(string s) {
            foreach (char c in s)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return s.Length > 0;
        }
    }
}
