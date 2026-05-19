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
    public class Emulator : IDisposable
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

        // External-interrupt injection used by the PLIC peripheral.
        public static void SetMachineExtIrq(bool level)    => rv32i_set_meip(level ? 1 : 0);

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
            const int chunk = 100_000;
            int total = 0;
            while (total < n && !_cachedHalted)
            {
                _clint?.Tick();
                int want = n - total < chunk ? n - total : chunk;
                int r = rv32i_step_n(want);
                total += r < 0 ? -r : r;
                if (r <= 0) { SyncHalted(); break; }
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
