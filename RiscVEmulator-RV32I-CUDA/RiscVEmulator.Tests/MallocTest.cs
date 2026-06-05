using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace RiscVEmulator.Tests;

[TestClass]
public class MallocTest : EmulatorTestBase
{
    private static string? _output;

    [ClassInitialize]
    public static void Build(TestContext ctx)
    {
        string rtObj = EnsureRuntimeObject();
        string mallocObj = EnsureMallocObject();

        string srcFile = Path.Combine(ProgramDir, "malloc_test.c");
        string elfFile = Path.Combine(TestDir, "malloc_test.elf");
        CompileC(new[] { srcFile, mallocObj, rtObj }, elfFile);

        var (output, exitCode, halted) = RunElf(elfFile, 20_000_000);
        Assert.IsTrue(halted, "emulator did not halt");
        Assert.AreEqual(0, exitCode, $"exit code {exitCode}");
        _output = output;
    }

    private static void AssertOk(string label)
        => StringAssert.Contains(_output, $"{label}: OK", $"'{label}' not OK in output");

    [TestMethod] public void MallocNonNull()      => AssertOk("malloc_nonnull");
    [TestMethod] public void MallocReadWrite()     => AssertOk("malloc_rw");
    [TestMethod] public void Malloc2NonNull()      => AssertOk("malloc2_nonnull");
    [TestMethod] public void Malloc2Different()    => AssertOk("malloc2_different");
    [TestMethod] public void ReuseNonNull()        => AssertOk("reuse_nonnull");
    [TestMethod] public void ReuseAddr()           => AssertOk("reuse_addr");
    [TestMethod] public void CallocNonNull()       => AssertOk("calloc_nonnull");
    [TestMethod] public void CallocZeroed()        => AssertOk("calloc_zeroed");
    [TestMethod] public void ReallocNonNull()      => AssertOk("realloc_nonnull");
    [TestMethod] public void ReallocPreserved()    => AssertOk("realloc_preserved");
    [TestMethod] public void ManyAllocs()          => AssertOk("many_allocs");
    [TestMethod] public void PostFreeAlloc()       => AssertOk("post_free_alloc");
    [TestMethod] public void MallocZero()          => AssertOk("malloc_zero");
    [TestMethod] public void FreeNull()            => AssertOk("free_null");

    [TestMethod]
    public void CompletionMessage()
        => StringAssert.Contains(_output, "malloc_test: done");
}
