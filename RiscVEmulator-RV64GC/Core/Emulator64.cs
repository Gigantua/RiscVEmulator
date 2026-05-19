using System.Runtime.InteropServices;
using RiscVEmulator.Core.Peripherals;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// RV64GC emulator — the emulator's single CPU front-end. Binds the
    /// <c>rv64gc_core</c> native CPU; PC and registers are 64-bit. The native
    /// CPU just reads and writes memory at host_base + guest_addr, with no
    /// peripheral knowledge.
    ///
    /// The host-side memory model is unchanged. An RV64 machine here keeps RAM
    /// at 0x80000000 with ≤2 GB of it, so every guest physical address still
    /// fits inside the existing 4 GB <see cref="HostMemoryReservation"/> — the
    /// reservation, <see cref="MemoryBus"/>, peripherals and
    /// <see cref="MmioDispatcher"/> are reused verbatim.
    /// </summary>
    public class Emulator64 : IDisposable
    {
        private const string Lib = "rv64gc_core";

        [DllImport(Lib)] private static extern void  rv64_init(IntPtr mem, ulong entry);
        [DllImport(Lib)] private static extern void  rv64_destroy();
        [DllImport(Lib)] private static extern int   rv64_step_n(int n);
        [DllImport(Lib)] private static extern ulong rv64_get_pc();
        [DllImport(Lib)] private static extern void  rv64_set_reg(int index, ulong value);
        [DllImport(Lib)] private static extern int   rv64_is_halted();
        [DllImport(Lib)] private static extern void  rv64_set_halted(int value);
        [DllImport(Lib)] private static extern ulong rv64_get_priv_mode();
        [DllImport(Lib)] private static extern uint  rv64_get_mtime_lo();
        [DllImport(Lib)] private static extern uint  rv64_get_mtime_hi();
        [DllImport(Lib)] private static extern ulong rv64_get_mtimecmp();
        [DllImport(Lib)] private static extern void  rv64_set_meip(int level);
        [DllImport(Lib)] private static extern void  rv64_set_seip(int level);
        [DllImport(Lib)] private static extern void  rv64_set_sbi_mode(int on);
        [DllImport(Lib)] private static extern ulong rv64_dbg(uint which);

        // External-interrupt injection used by the PLIC peripheral.
        public static void SetMachineExtIrq(bool level)    => rv64_set_meip(level ? 1 : 0);
        public static void SetSupervisorExtIrq(bool level) => rv64_set_seip(level ? 1 : 0);

        /// <summary>
        /// Boot the guest directly in S-mode with the emulator servicing SBI
        /// ecalls — the mode a real rv64gc Linux kernel expects. Call once after
        /// constructing the <see cref="Emulator64"/> and before the first
        /// <see cref="StepN"/>.
        /// </summary>
        public void EnableSbiMode() => rv64_set_sbi_mode(1);

        /// <summary>Diagnostic peek into native CPU/SBI counters (see rv64_dbg).</summary>
        public ulong Dbg(uint which) => rv64_dbg(which);

        // ── State ────────────────────────────────────────────────────────────

        private readonly HostMemoryReservation _reservation;
        private readonly HostExitDevice _exitDevice = new();
        private bool _disposed;
        private bool _cachedHalted;
        private int  _cachedExitCode;

        public bool  IsHalted => _cachedHalted;
        public int   ExitCode => _cachedExitCode;
        public ulong PC       => rv64_get_pc();
        public ulong MTime    => (ulong)rv64_get_mtime_lo() | ((ulong)rv64_get_mtime_hi() << 32);
        public ulong MtimeCmp => rv64_get_mtimecmp();
        public ulong PrivMode => rv64_get_priv_mode();

        public uint RamOffset { get; set; }
        public Action<char>? OutputHandler { get; set; }

        // ── Constructor ──────────────────────────────────────────────────────

        public unsafe Emulator64(IMemoryBus bus, RegisterFile registers, ulong entryPoint)
        {
            var memBus = (MemoryBus)bus;
            _reservation = memBus.RawMemory.Reservation;

            _exitDevice.OnExit = code =>
            {
                _cachedExitCode = code;
                rv64_set_halted(1);
            };

            foreach (var p in bus.Peripherals) CommitPeripheral(p);
            CommitPeripheral(_exitDevice);

            rv64_init(_reservation.Base, entryPoint);

            for (int i = 1; i < 32; i++)
            {
                uint val = registers.Read(i);
                if (val != 0) rv64_set_reg(i, val);
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
            rv64_set_halted(value ? 1 : 0);
            _cachedHalted = value;
        }

        public bool Step()
        {
            if (_cachedHalted) return false;
            int r = rv64_step_n(1);
            if (r <= 0) SyncHalted();
            return !_cachedHalted;
        }

        public int StepN(int n)
        {
            if (_cachedHalted) return 0;
            int r = rv64_step_n(n);
            int executed = r < 0 ? -r : r;
            if (r <= 0) SyncHalted();
            return executed;
        }

        private void SyncHalted()
        {
            _cachedHalted = rv64_is_halted() != 0;
        }

        public void Dispose()
        {
            if (_disposed) return;
            rv64_destroy();
            MmioDispatcher.Clear();
            _reservation.Dispose();
            _disposed = true;
            GC.SuppressFinalize(this);
        }
    }
}
