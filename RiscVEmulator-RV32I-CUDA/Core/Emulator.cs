using System.Runtime.InteropServices;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// RV32I emulator. The native CPU just reads and writes memory at
    /// host_base + guest_addr — no peripheral knowledge, no callbacks.
    ///
    /// The Emulator owns a <see cref="HostMemoryReservation"/> covering the
    /// guest address space. Each peripheral on the bus commits a slice in it:
    ///   • plain (FB, audio PCM) → PAGE_READWRITE, CPU dereferences directly
    ///   • guarded (UART, KBD, …) → PAGE_NOACCESS; accesses raise AVs that
    ///     <see cref="MmioDispatcher"/> services by calling the peripheral's
    ///     Read / Write.
    /// </summary>
    public class Emulator : IDisposable, IEmulator
    {
        private const string Lib = "rv32i_core";

        [DllImport(Lib)] private static extern void rv32i_init(IntPtr mem, uint entry);
        [DllImport(Lib)] private static extern void rv32i_destroy();
        [DllImport(Lib)] private static extern int  rv32i_step_n(int n);
        [DllImport(Lib)] private static extern uint rv32i_get_pc();
        [DllImport(Lib)] private static extern void rv32i_set_reg(int index, uint value);
        [DllImport(Lib)] private static extern int  rv32i_is_halted();
        [DllImport(Lib)] private static extern void rv32i_set_halted(int value);
        [DllImport(Lib)] private static extern void rv32i_set_meip(int level);
        [DllImport(Lib)] private static extern void rv32i_set_softfloat_hook(int op, uint pc);

        // External-interrupt injection used by the PLIC peripheral.
        public static void SetMachineExtIrq(bool level)    => rv32i_set_meip(level ? 1 : 0);

        /// <summary>
        /// SSE softfloat shortcut ops. When the JIT translates a basic block
        /// whose entry PC matches a hook PC, it replaces the entire function
        /// body with inline x86 SSE (addss/mulss/etc.) and a jump back to ra.
        /// One libgcc-style softfloat call collapses from ~500 RV32 instructions
        /// to ~10 host x86 ops — multi-x speedup on float-heavy workloads.
        /// </summary>
        public enum SoftFloatOp {
            None    = 0,
            AddSf3, SubSf3, MulSf3, DivSf3,
            AddDf3, SubDf3, MulDf3, DivDf3,
        }

        /// <summary>Register a softfloat ABI entry-point PC with the JIT.</summary>
        public static void SetSoftFloatHook(SoftFloatOp op, uint pc)
            => rv32i_set_softfloat_hook((int)op, pc);

        /// <summary>
        /// Auto-wire all known softfloat ABI entry points found in the
        /// supplied ELF symbol table. Returns the number of hooks installed.
        /// </summary>
        public static int InstallSoftFloatHooks(Dictionary<string, uint> symbols)
        {
            int installed = 0;
            (string name, SoftFloatOp op)[] map = {
                ("__addsf3", SoftFloatOp.AddSf3),
                ("__subsf3", SoftFloatOp.SubSf3),
                ("__mulsf3", SoftFloatOp.MulSf3),
                ("__divsf3", SoftFloatOp.DivSf3),
                ("__adddf3", SoftFloatOp.AddDf3),
                ("__subdf3", SoftFloatOp.SubDf3),
                ("__muldf3", SoftFloatOp.MulDf3),
                ("__divdf3", SoftFloatOp.DivDf3),
            };
            foreach (var (name, op) in map)
            {
                if (symbols.TryGetValue(name, out uint pc) && pc != 0)
                {
                    SetSoftFloatHook(op, pc);
                    installed++;
                }
            }
            return installed;
        }

        // ── State ────────────────────────────────────────────────────────────

        private readonly HostMemoryReservation _reservation;
        private readonly HostExitDevice _exitDevice = new();
        private readonly ClintDevice? _clint;
        private bool _disposed;
        private bool _cachedHalted;
        private int  _cachedExitCode;

        public bool IsHalted => _cachedHalted;
        public int  ExitCode => _cachedExitCode;
        public uint PC       => rv32i_get_pc();
        public ulong MTime    => _clint?.Mtime ?? 0;

        public Action<char>? OutputHandler { get; set; }

        // ── Constructor ──────────────────────────────────────────────────────

        public unsafe Emulator(IMemoryBus bus, RegisterFile registers, uint entryPoint)
        {
            var memBus = (MemoryBus)bus;
            _reservation = memBus.RawMemory.Reservation;

            // Commit each peripheral's slice and wire it up.
            _exitDevice.OnExit = code =>
            {
                _cachedExitCode = code;
                rv32i_set_halted(1);
            };

            foreach (var p in bus.Peripherals)
            {
                CommitPeripheral(p);
                if (p is ClintDevice clint) _clint = clint;
            }
            CommitPeripheral(_exitDevice);

            rv32i_init(_reservation.Base, entryPoint);

            for (int i = 1; i < 32; i++)
            {
                uint val = registers.Read(i);
                if (val != 0) rv32i_set_reg(i, val);
            }
        }

        private unsafe void CommitPeripheral(IPeripheral p)
        {
            IntPtr slice;
            if (p.IsGuarded)
            {
                slice = _reservation.CommitGuarded(p.BaseAddress, p.Size);
                MmioDispatcher.Register(slice, ((p.Size + 0xFFFu) & ~0xFFFu), p);
            }
            else
            {
                slice = _reservation.CommitPlain(p.BaseAddress, p.Size);
                p.Bind((byte*)slice);
            }
        }

        // ── Public run API ───────────────────────────────────────────────────

        public void SetHalted(bool value)
        {
            rv32i_set_halted(value ? 1 : 0);
            _cachedHalted = value;
        }

        public bool Step()
        {
            if (_cachedHalted) return false;
            _clint?.Tick();
            int r = rv32i_step_n(1);
            if (r <= 0) SyncHalted();
            return !_cachedHalted;
        }

        public int StepN(int n)
        {
            if (_cachedHalted) return 0;
            // Run in sub-batches so the CLINT timer is polled often enough to
            // keep MTIP latency bounded regardless of the caller's batch size.
            // Linux needs MTIP latency ≤ a few thousand instructions during
            // very early boot (timer-IPI handshake before scheduler is up) —
            // 100k is the proven-working value; bumping it broke Linux kernel
            // boot. Per-example loops (Quake's SdlWindow CpuThread) batch
            // their own StepN calls to amortize the C# round-trip.
            const int chunk = 100_000;
            int total = 0;
            //
            // CRITICAL: suppress GC for the duration of each JIT batch.
            //
            // While `rv32i_step_n` is running, the program counter sits inside
            // the JIT-emitted RWX arena. Our JIT does NOT publish SEH unwind
            // metadata (no .pdata/.xdata table, no RtlInstallFunctionTable
            // entry). If the .NET GC happens to fire while a JIT frame is on
            // the stack — say from an MMIO peripheral's `Console.Write` or a
            // VEH-dispatched callback — its stack walker can't unwind past
            // the JIT frame and Windows fast-fails the process with
            // STATUS_STACK_BUFFER_OVERRUN (0xc0000409). Symptom under
            // Linux: kernel oops printk burst → host dies.
            //
            // GC.TryStartNoGCRegion with a generous budget pins the heap
            // for the batch duration. We use `disallowFullBlockingGC: true`
            // so a blocking gen-2 fallback won't sneak in. The 64 MiB
            // budget is well above any single batch's allocation pressure
            // (drain thread + SDL renderer allocate freely between batches).
            const long NoGcBudget = 256L * 1024 * 1024;
            bool noGc = false;
            try
            {
                // disallowFullBlockingGC: TRUE — even on budget exceed, no
                // blocking GC fires. Only background GCs (which don't suspend
                // running threads) are permitted. That eliminates the
                // suspend-and-walk-JIT race entirely for the JIT batch.
                noGc = System.GC.TryStartNoGCRegion(NoGcBudget, disallowFullBlockingGC: true);
            }
            catch (InvalidOperationException)
            {
                // Already in a no-GC region (nested StepN) — that's fine.
            }
            try
            {
                while (total < n && !_cachedHalted)
                {
                    _clint?.Tick();
                    int want = n - total < chunk ? n - total : chunk;
                    int r = rv32i_step_n(want);
                    total += r < 0 ? -r : r;
                    if (r <= 0) { SyncHalted(); break; }
                }
            }
            finally
            {
                if (noGc)
                {
                    try { System.GC.EndNoGCRegion(); }
                    catch (InvalidOperationException)
                    {
                        // Region was exited (allocation exceeded budget). Force
                        // a collection so the heap settles before the next batch.
                        System.GC.Collect(0, System.GCCollectionMode.Forced, blocking: true);
                    }
                }
            }
            return total;
        }

        public void Run(int maxSteps = 20_000_000)
        {
            int remaining = maxSteps;
            const int batchSize = 250_000;
            while (remaining > 0 && !_cachedHalted)
            {
                int batch    = remaining < batchSize ? remaining : batchSize;
                int executed = StepN(batch);
                if (executed == 0) break;
                remaining   -= executed;
            }
            SyncHalted();
        }

        private void SyncHalted()
        {
            _cachedHalted = rv32i_is_halted() != 0;
        }

        public void Dispose()
        {
            if (_disposed) return;
            rv32i_destroy();
            MmioDispatcher.Clear();
            _reservation.Dispose();
            _disposed = true;
            GC.SuppressFinalize(this);
        }
    }
}
