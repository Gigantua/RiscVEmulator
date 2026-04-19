using System.Runtime.InteropServices;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// RV32I+M emulator backed by native C++ hot path (rv32i_core).
    /// RAM is pinned and shared zero-copy between C# and native.
    /// MMIO and ECALL route back to managed code via delegates.
    /// </summary>
    public class Emulator : IDisposable
    {
        // ── P/Invoke ─────────────────────────────────────────────────────────

        private const string Lib = "rv32i_core";

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint MmioReadDelegate(uint address, int width);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void MmioWriteDelegate(uint address, int width, uint value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void EcallDelegate(IntPtr regs);

        [DllImport(Lib)] private static extern void rv32i_init(IntPtr ram, uint ramSize, uint entry, int mExt);
        [DllImport(Lib)] private static extern void rv32i_destroy();
        [DllImport(Lib)] private static extern void rv32i_set_mmio(MmioReadDelegate read, MmioWriteDelegate write);
        [DllImport(Lib)] private static extern void rv32i_set_ecall(EcallDelegate handler);
        [DllImport(Lib)] private static extern int  rv32i_step_n(int n);
        [DllImport(Lib)] private static extern uint rv32i_get_pc();
        [DllImport(Lib)] private static extern void rv32i_set_reg(int index, uint value);
        [DllImport(Lib)] private static extern int  rv32i_is_halted();
        [DllImport(Lib)] private static extern void rv32i_set_halted(int value);
        [DllImport(Lib)] private static extern int  rv32i_exit_code();
        [DllImport(Lib)] private static extern void rv32i_set_exit_code(int code);
        [DllImport(Lib)] private static extern void rv32i_set_m_ext(int value);
        [DllImport(Lib)] private static extern void rv32i_set_f_ext(int value);
        [DllImport(Lib)] private static extern void rv32i_set_a_ext(int value);
        [DllImport(Lib)] private static extern void rv32i_set_priv_mode(int value);
        [DllImport(Lib)] private static extern void rv32i_set_ram_offset(uint value);
        [DllImport(Lib)] private static extern uint rv32i_get_mtime_lo();
        [DllImport(Lib)] private static extern uint rv32i_get_mtime_hi();
        [DllImport(Lib)] private static extern uint rv32i_get_priv_mode();
        [DllImport(Lib)] private static extern uint rv32i_get_umode_lo();
        [DllImport(Lib)] private static extern uint rv32i_get_umode_hi();
        [DllImport(Lib)] private static extern uint rv32i_get_last_trap();
        [DllImport(Lib)] private static extern uint rv32i_get_trap_count();
        [DllImport(Lib)] private static extern uint rv32i_get_mret_to_u();
        [DllImport(Lib)] private static extern uint rv32i_get_mret_to_m();
        [DllImport(Lib)] private static extern uint rv32i_get_uecall_count();

        // ── Callback delegates (static so GC can't collect them) ─────────────

        private static readonly MmioReadDelegate  s_mmioReadDel  = MmioReadCallback;
        private static readonly MmioWriteDelegate s_mmioWriteDel = MmioWriteCallback;
        private static readonly EcallDelegate     s_ecallDel     = EcallCallback;

        // ── Shared state for callbacks (single CPU instance) ─────────────────

        private static IMemoryBus? s_bus;
        private static Action<char>? s_outputHandler;
        private static byte[]? s_ramData;
        private static uint s_ramSize;

        // ── Instance state ───────────────────────────────────────────────────

        private GCHandle _ramHandle;
        private bool _disposed;
        private bool _cachedHalted;
        private int  _cachedExitCode;

        // ── Properties ───────────────────────────────────────────────────────

        public bool IsHalted => _cachedHalted;
        public int  ExitCode => _cachedExitCode;
        public uint PC       => rv32i_get_pc();

        private bool _enableMExt;
        /// <summary>When true, M-extension instructions (MUL/DIV/REM) are enabled.</summary>
        public bool EnableMExtension
        {
            get => _enableMExt;
            set { _enableMExt = value; rv32i_set_m_ext(value ? 1 : 0); }
        }

        private bool _enableFExt;
        /// <summary>When true, F-extension instructions (single-precision float) are enabled.</summary>
        public bool EnableFExtension
        {
            get => _enableFExt;
            set { _enableFExt = value; rv32i_set_f_ext(value ? 1 : 0); }
        }

        private bool _enableAExt;
        /// <summary>When true, A-extension atomic instructions (LR.W, SC.W, AMO*) are enabled.</summary>
        public bool EnableAExtension
        {
            get => _enableAExt;
            set { _enableAExt = value; rv32i_set_a_ext(value ? 1 : 0); }
        }

        private bool _enablePrivMode;
        /// <summary>
        /// When true, full M-mode privileged infrastructure is enabled:
        /// CSRs, trap dispatch, timer interrupt, MRET/SRET/WFI, and Zicsr.
        /// Required to boot Linux. ECALL no longer calls the C# callback in this mode —
        /// the SBI stub inside the kernel image handles it as native RISC-V code.
        /// </summary>
        public bool EnablePrivMode
        {
            get => _enablePrivMode;
            set { _enablePrivMode = value; rv32i_set_priv_mode(value ? 1 : 0); }
        }

        private uint _ramOffset;
        /// <summary>
        /// Physical base address of RAM. Set to 0x80000000 when booting Linux
        /// so that ELF/flat images linked at 0x80000000 index RAM correctly.
        /// Default is 0 (bare-metal layout).
        /// </summary>
        public uint RamOffset
        {
            get => _ramOffset;
            set { _ramOffset = value; rv32i_set_ram_offset(value); }
        }

        /// <summary>Current emulator mtime counter (64-bit, instruction-count clock).</summary>
        public ulong MTime =>
            (ulong)rv32i_get_mtime_lo() | ((ulong)rv32i_get_mtime_hi() << 32);

        /// <summary>Current privilege mode: 0=U, 1=S, 3=M.</summary>
        public uint PrivMode => rv32i_get_priv_mode();

        /// <summary>Total instructions executed while in U-mode (user-space).</summary>
        public ulong UModeCount =>
            (ulong)rv32i_get_umode_lo() | ((ulong)rv32i_get_umode_hi() << 32);

        /// <summary>Most recent do_trap cause code.</summary>
        public uint LastTrap    => rv32i_get_last_trap();
        /// <summary>Total number of do_trap invocations.</summary>
        public uint TrapCount   => rv32i_get_trap_count();
        /// <summary>MRET instructions that returned to U-mode (mpp=0).</summary>
        public uint MRetToU     => rv32i_get_mret_to_u();
        /// <summary>MRET instructions that returned to M-mode (mpp=3).</summary>
        public uint MRetToM     => rv32i_get_mret_to_m();
        /// <summary>ECALL instructions from U-mode (Linux user syscalls).</summary>
        public uint UEcallCount => rv32i_get_uecall_count();

        /// <summary>Called with each character output via write syscall (ecall a7=64).</summary>
        public Action<char>? OutputHandler
        {
            get => s_outputHandler;
            set => s_outputHandler = value;
        }

        // ── Constructor ──────────────────────────────────────────────────────

        public Emulator(IMemoryBus bus, RegisterFile registers, uint entryPoint)
        {
            var memBus  = (MemoryBus)bus;
            byte[] ramData = memBus.RawMemory.Data;

            _ramHandle = GCHandle.Alloc(ramData, GCHandleType.Pinned);
            s_bus      = bus;
            s_ramData  = ramData;
            s_ramSize  = (uint)ramData.Length;

            rv32i_init(_ramHandle.AddrOfPinnedObject(), s_ramSize, entryPoint, 0);

            // Copy any pre-set registers (typically just SP = x2)
            for (int i = 1; i < 32; i++)
            {
                uint val = registers.Read(i);
                if (val != 0) rv32i_set_reg(i, val);
            }

            rv32i_set_mmio(s_mmioReadDel, s_mmioWriteDel);
            rv32i_set_ecall(s_ecallDel);
        }

        // ── MMIO callbacks ───────────────────────────────────────────────────

        private static uint MmioReadCallback(uint address, int width)
        {
            if (s_bus is null) return 0;
            return width switch
            {
                1 => s_bus.ReadByte(address),
                2 => s_bus.ReadHalfWord(address),
                4 => s_bus.ReadWord(address),
                _ => 0
            };
        }

        private static void MmioWriteCallback(uint address, int width, uint value)
        {
            if (s_bus is null) return;
            switch (width)
            {
                case 1: s_bus.WriteByte(address, (byte)value); break;
                case 2: s_bus.WriteHalfWord(address, (ushort)value); break;
                case 4: s_bus.WriteWord(address, value); break;
            }
        }

        // ── ECALL callback ───────────────────────────────────────────────────

        private static void EcallCallback(IntPtr regs)
        {
            uint a7 = (uint)Marshal.ReadInt32(regs, 17 * 4);
            switch (a7)
            {
                case 93: // exit
                case 94: // exit_group
                    rv32i_set_exit_code(Marshal.ReadInt32(regs, 10 * 4));
                    rv32i_set_halted(1);
                    break;

                case 64: // write(fd, buf, len)
                {
                    uint buf = (uint)Marshal.ReadInt32(regs, 11 * 4);
                    uint len = (uint)Marshal.ReadInt32(regs, 12 * 4);
                    if (s_outputHandler is not null && s_ramData is not null)
                    {
                        for (uint i = 0; i < len; i++)
                        {
                            byte b = (buf + i < s_ramSize) ? s_ramData[buf + i] : (byte)0;
                            s_outputHandler((char)b);
                        }
                    }
                    Marshal.WriteInt32(regs, 10 * 4, (int)len); // return value = bytes written
                    break;
                }

                case 63: // read(fd, buf, len)
                {
                    uint buf   = (uint)Marshal.ReadInt32(regs, 11 * 4);
                    uint len   = (uint)Marshal.ReadInt32(regs, 12 * 4);
                    uint count = 0;
                    if (s_bus is not null && s_ramData is not null)
                    {
                        for (uint i = 0; i < len; i++)
                        {
                            // LSR bit 0 = RX data ready
                            if ((s_bus.ReadByte(0x10000005u) & 1) == 0) break;
                            byte b = s_bus.ReadByte(0x10000000u);
                            if (buf + i < s_ramSize) s_ramData[buf + i] = b;
                            count++;
                        }
                    }
                    Marshal.WriteInt32(regs, 10 * 4, (int)count);
                    break;
                }
            }
        }

        // ── Public run API ───────────────────────────────────────────────────

        /// <summary>Forcibly halt the emulator (e.g. called by a SYSCON poweroff peripheral).</summary>
        public void SetHalted(bool value)
        {
            rv32i_set_halted(value ? 1 : 0);
            _cachedHalted = value;
        }

        /// <summary>Execute one instruction. Returns false when halted.</summary>
        public bool Step()
        {
            if (_cachedHalted) return false;
            int r = rv32i_step_n(1);
            if (r <= 0) { SyncHalted(); return false; }
            return true;
        }

        /// <summary>Execute up to n instructions in a single native call. Returns actual steps executed.</summary>
        public int StepN(int n)
        {
            if (_cachedHalted) return 0;
            int r = rv32i_step_n(n);
            int executed = r < 0 ? -r : r;
            if (r <= 0) SyncHalted();
            return executed;
        }

        /// <summary>Run until halted or maxSteps exceeded.</summary>
        public void Run(int maxSteps = 20_000_000)
        {
            if (!_cachedHalted)
                rv32i_step_n(maxSteps);
            SyncHalted();
        }

        private void SyncHalted()
        {
            _cachedHalted = rv32i_is_halted() != 0;
            if (_cachedHalted)
                _cachedExitCode = rv32i_exit_code();
        }

        // ── Dispose ──────────────────────────────────────────────────────────

        public void Dispose()
        {
            if (_disposed) return;
            rv32i_destroy();
            if (_ramHandle.IsAllocated) _ramHandle.Free();
            s_bus = null;
            s_outputHandler = null;
            s_ramData = null;
            _disposed = true;
            GC.SuppressFinalize(this);
        }

        ~Emulator()
        {
            if (_ramHandle.IsAllocated) _ramHandle.Free();
        }
    }
}
