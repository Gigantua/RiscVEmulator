namespace RiscVEmulator.Core;

/// <summary>
/// Single source of truth for the RISC-V guest toolchain target.
///
/// Every clang/gcc invocation in the solution — the integration tests, the
/// bare-metal <c>Examples</c>, and the buildroot driver that produces the
/// Linux kernel + busybox + .ipk packages — derives its <c>--target</c>,
/// <c>-march</c> and <c>-mabi</c> from the constants here. Changing the
/// emulated CPU is therefore a one-file edit: adjust the values below and
/// rebuild; every distributed compiler invocation follows.
///
/// The native core (<c>Native/rv64gc_core.cpp</c>) implements RV64 I + M + A +
/// F + D + C + Zicsr/Zifencei + the Sv39 MMU. The ISA strings below stay a
/// subset of that envelope so anything built with them runs on the emulator.
///
/// This class only covers the knobs C# can reach. Several CPU-feature flags
/// live outside it — the native core itself, the buildroot kernel config,
/// device trees, linker scripts. <c>ISA_PLAYBOOK.md</c> at the repo root is
/// the end-to-end procedure: consult it whenever you add or change a CPU
/// feature.
/// </summary>
public static class RiscVTarget
{
    /// <summary>clang/gcc bare-metal target triple for guest programs.</summary>
    public const string Triple = "riscv64-unknown-elf";

    /// <summary>Calling-convention ABI: 64-bit int/long/pointer, hardware-double.</summary>
    public const string Abi = "lp64d";

    /// <summary>
    /// -march for guest programs. The RV64GC core implements every extension
    /// the toolchain emits here — I, M, A, F, D and C — so all three Arch*
    /// values are the full ISA. The split is kept only so call sites read
    /// intentionally; there is no longer a reduced subset.
    /// </summary>
    public const string ArchBase = "rv64gc";

    /// <summary>-march for programs using the M extension (hardware mul/div).</summary>
    public const string ArchM = "rv64gc";

    /// <summary>-march covering every extension the emulator implements.</summary>
    public const string ArchFull = "rv64gc";

    /// <summary>
    /// The <c>--target / -march / -mabi</c> triple as clang/gcc arguments.
    /// Prepend to any guest compile or link command line.
    /// </summary>
    /// <param name="march">An <c>Arch*</c> value selecting the ISA extensions.</param>
    public static string[] ClangTargetArgs(string march) =>
        [$"--target={Triple}", $"-march={march}", $"-mabi={Abi}"];

    /// <summary>
    /// buildroot <c>BR2_*</c> ISA/ABI config symbols, kept in lockstep with
    /// the <c>Arch*</c> strings above. The buildroot driver
    /// (<c>Examples.Linux.Build_RV32i</c>) writes these into <c>.config</c> so
    /// the cross-toolchain it builds — and therefore busybox, the kernel and
    /// every .ipk package — targets exactly the emulated CPU.
    ///
    /// RVF/RVD are off: the emulator has hardware single-float, but the guest
    /// rootfs is built soft-float for ABI uniformity with uclibc/musl. RVC is
    /// off because the core has no compressed-instruction decoder.
    /// </summary>
    public static readonly (string Symbol, bool Enabled)[] BuildrootIsa =
    [
        ("BR2_RISCV_ISA_RVI", true),    // base integer
        ("BR2_RISCV_ISA_RVM", true),    // multiply / divide
        ("BR2_RISCV_ISA_RVA", true),    // atomics (LR/SC, AMO)
        ("BR2_RISCV_ISA_RVF", false),   // single-precision float
        ("BR2_RISCV_ISA_RVD", false),   // double-precision float
        ("BR2_RISCV_ISA_RVC", false),   // compressed instructions
    ];

    /// <summary>
    /// buildroot defconfig the Linux image build starts from. nommu vs. MMU
    /// is a machine-level decision (it pulls in a different libc, executable
    /// format and boot model), not only an ISA one — see <c>ISA_PLAYBOOK.md</c>.
    /// </summary>
    public const string BuildrootDefconfig = "qemu_riscv32_nommu_virt_defconfig";

    /// <summary>
    /// Architecture tag stamped into <c>.ipk</c> packages and the feed index
    /// by <c>Examples.Linux.Packageserver</c>. Encodes ISA + MMU + libc so a
    /// package can never be installed onto an incompatible rootfs. Changing
    /// it invalidates the existing feed cache (every package must rebuild).
    /// </summary>
    public const string FeedArch = "rv32inommu_uclibc";

    /// <summary>
    /// Renders <see cref="BuildrootIsa"/> plus the ABI selector as the lines
    /// a buildroot <c>.config</c> expects (<c>SYM=y</c> / <c># SYM is not set</c>).
    /// </summary>
    public static string BuildrootConfigLines()
    {
        var sb = new System.Text.StringBuilder();
        sb.Append("BR2_riscv_custom=y\n");
        foreach (var (sym, on) in BuildrootIsa)
            sb.Append(on ? $"{sym}=y\n" : $"# {sym} is not set\n");
        bool ilp32 = Abi == "ilp32";
        sb.Append(ilp32 ? "BR2_RISCV_ABI_ILP32=y\n"  : "# BR2_RISCV_ABI_ILP32 is not set\n");
        sb.Append(ilp32 ? "# BR2_RISCV_ABI_ILP32D is not set\n" : "BR2_RISCV_ABI_ILP32D=y\n");
        return sb.ToString();
    }
}
