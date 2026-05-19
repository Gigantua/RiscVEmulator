using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class VfsTest : EmulatorTestBase
{
    private static string? _output;

    [ClassInitialize]
    public static void Build(TestContext ctx)
    {
        string rtObj = EnsureRuntimeObject();
        string vfsObj = EnsureVfsObject();

        string srcFile = Path.Combine(ProgramDir, "vfs_test.c");
        string elfFile = Path.Combine(TestDir, "vfs_test.elf");
        CompileC(new[] { srcFile, vfsObj, rtObj }, elfFile);

        var (output, exitCode, halted) = RunElf(elfFile, 20_000_000);
        Assert.IsTrue(halted, "emulator did not halt");
        Assert.AreEqual(0, exitCode, $"exit code {exitCode}");
        _output = output;
    }

    private static void AssertOk(string label)
        => StringAssert.Contains(_output, $"{label}: OK", $"'{label}' not OK in output");

    [TestMethod] public void OpenOk()           => AssertOk("open_ok");
    [TestMethod] public void TellStart()        => AssertOk("tell_start");
    [TestMethod] public void EofStartNo()       => AssertOk("eof_start_no");
    [TestMethod] public void ReadCount7()       => AssertOk("read_count_7");
    [TestMethod] public void ReadH()            => AssertOk("read_H");
    [TestMethod] public void ReadE()            => AssertOk("read_e");
    [TestMethod] public void ReadCommaSpace()   => AssertOk("read_comma_space");
    [TestMethod] public void Tell7()            => AssertOk("tell_7");
    [TestMethod] public void Read2Count()       => AssertOk("read2_count");
    [TestMethod] public void Read2W()           => AssertOk("read2_W");
    [TestMethod] public void Read2Newline()     => AssertOk("read2_newline");
    [TestMethod] public void Tell14()           => AssertOk("tell_14");
    [TestMethod] public void Read3Count()       => AssertOk("read3_count");
    [TestMethod] public void Read3DE()          => AssertOk("read3_DE");
    [TestMethod] public void Read3EF()          => AssertOk("read3_EF");
    [TestMethod] public void EofYes()           => AssertOk("eof_yes");
    [TestMethod] public void ReadEof0()         => AssertOk("read_eof_0");
    [TestMethod] public void SeekSetOk()        => AssertOk("seek_set_ok");
    [TestMethod] public void SeekSetPos()       => AssertOk("seek_set_pos");
    [TestMethod] public void SeekSetNoEof()     => AssertOk("seek_set_noeof");
    [TestMethod] public void RereadH()          => AssertOk("reread_H");
    [TestMethod] public void SeekCurPos()       => AssertOk("seek_cur_pos");
    [TestMethod] public void SeekCurW()         => AssertOk("seek_cur_W");
    [TestMethod] public void SeekEndPos()       => AssertOk("seek_end_pos");
    [TestMethod] public void SeekEndDE()        => AssertOk("seek_end_DE");
    [TestMethod] public void OpenNoExistNull()  => AssertOk("open_noexist_null");
    [TestMethod] public void CloseOk()          => AssertOk("close_ok");
    [TestMethod] public void OpenByBasename()   => AssertOk("open_by_basename");
    [TestMethod] public void BasenameRead()     => AssertOk("basename_read");

    [TestMethod]
    public void CompletionMessage()
        => StringAssert.Contains(_output, "vfs_test: done");
}
