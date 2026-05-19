using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class SyscallsTest : EmulatorTestBase
{
    private static string? _output;

    [ClassInitialize]
    public static void Build(TestContext ctx)
    {
        string rtObj = EnsureRuntimeObject();
        string scObj = EnsureSyscallsObject();

        string srcFile = Path.Combine(ProgramDir, "syscalls_test.c");
        string elfFile = Path.Combine(TestDir, "syscalls_test.elf");
        CompileC(new[] { srcFile, scObj, rtObj }, elfFile);

        var (output, exitCode, halted) = RunElf(elfFile, 20_000_000);
        Assert.IsTrue(halted, "emulator did not halt");
        Assert.AreEqual(0, exitCode, $"exit code {exitCode}");
        _output = output;
    }

    private static void AssertOk(string label)
        => StringAssert.Contains(_output, $"{label}: OK", $"'{label}' not OK in output");

    [TestMethod] public void WriteStdoutRet()       => AssertOk("write_stdout_ret");
    [TestMethod] public void WriteStderrRet()       => AssertOk("write_stderr_ret");
    [TestMethod] public void WriteBadFd()           => AssertOk("write_badfd");
    [TestMethod] public void ReadEof()              => AssertOk("read_eof");
    [TestMethod] public void OpenFail()             => AssertOk("open_fail");
    [TestMethod] public void CloseStub()            => AssertOk("close_stub");
    [TestMethod] public void LseekZero()            => AssertOk("lseek_zero");
    [TestMethod] public void IsattyStdout()         => AssertOk("isatty_stdout");
    [TestMethod] public void IsattyStdin()          => AssertOk("isatty_stdin");
    [TestMethod] public void FstatRet()             => AssertOk("fstat_ret");
    [TestMethod] public void FstatMode()            => AssertOk("fstat_mode");
    [TestMethod] public void Getpid1()              => AssertOk("getpid_1");
    [TestMethod] public void SbrkNonnull()          => AssertOk("sbrk_nonnull");
    [TestMethod] public void SbrkReturnsPrev()      => AssertOk("sbrk_returns_prev");
    [TestMethod] public void SbrkAdvanced()         => AssertOk("sbrk_advanced");
    [TestMethod] public void SbrkSecond()           => AssertOk("sbrk_second");
    [TestMethod] public void SbrkTotal()            => AssertOk("sbrk_total");
    [TestMethod] public void GettimeofdayRet()      => AssertOk("gettimeofday_ret");
    [TestMethod] public void GettimeofdaySec()      => AssertOk("gettimeofday_sec");
    [TestMethod] public void GettimeofdayUsec()     => AssertOk("gettimeofday_usec");
    [TestMethod] public void GettimeofdayAdvances() => AssertOk("gettimeofday_advances");
    [TestMethod] public void WriteZero()            => AssertOk("write_zero");

    [TestMethod]
    public void CompletionMessage()
        => StringAssert.Contains(_output, "syscalls_test: done");
}
