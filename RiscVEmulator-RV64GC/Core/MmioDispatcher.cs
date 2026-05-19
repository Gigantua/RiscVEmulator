using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace RiscVEmulator.Core
{
    /// <summary>
    /// Process-wide vectored exception handler that turns access-violations on
    /// guarded peripheral pages into synchronous <see cref="IPeripheral.Read"/> /
    /// <see cref="IPeripheral.Write"/> calls. Decodes the faulting x86 MOV at
    /// the fault PC, services the access, writes the result back into the
    /// saved register context, advances RIP past the instruction.
    /// </summary>
    public static unsafe class MmioDispatcher
    {
        private struct Range
        {
            public ulong       HostStart;   // inclusive
            public ulong       HostEnd;     // exclusive
            public uint        GuestBase;
            public IPeripheral Owner;
        }

        private static readonly List<Range> s_ranges = new();
        private static readonly object       s_lock   = new();
        private static IntPtr                s_vehHandle;

        private const int  EXCEPTION_CONTINUE_EXECUTION = -1;
        private const int  EXCEPTION_CONTINUE_SEARCH    = 0;
        private const uint EXCEPTION_ACCESS_VIOLATION   = 0xC0000005u;

        public static void Register(IntPtr hostStart, uint size, IPeripheral owner)
        {
            lock (s_lock)
            {
                EnsureInstalled();
                ulong start = (ulong)hostStart.ToInt64();
                s_ranges.Add(new Range
                {
                    HostStart = start,
                    HostEnd   = start + size,
                    GuestBase = owner.BaseAddress,
                    Owner     = owner,
                });
            }
        }

        public static void Clear()
        {
            lock (s_lock) s_ranges.Clear();
        }

        private static void EnsureInstalled()
        {
            if (s_vehHandle != IntPtr.Zero) return;
            // Use a function pointer (delegate*<...>) over &HandleException
            // rather than Marshal.GetFunctionPointerForDelegate(<UnmanagedFunctionPointer delegate>).
            // The legacy delegate path generates a reverse-pinvoke stub that
            // .NET 10 internally tags as UnmanagedCallersOnly — and the JIT
            // then refuses to invoke methods we transitively reach from inside
            // that stub (any P/Invoke nested below the VEH callback triggers
            // "Invalid Program: attempted to call a UnmanagedCallersOnly method
            // from managed code"). The explicit [UnmanagedCallersOnly] + raw
            // function pointer avoids the delegate stub entirely.
            delegate* unmanaged[Cdecl]<IntPtr, int> fp = &HandleException;
            s_vehHandle = AddVectoredExceptionHandler(1, (IntPtr)fp);
            if (s_vehHandle == IntPtr.Zero)
                throw new InvalidOperationException("AddVectoredExceptionHandler failed");
        }

        // ── Exception handler ────────────────────────────────────────────────

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static int HandleException(IntPtr exceptionInfo)
        {
            ExceptionPointers* ep = (ExceptionPointers*)exceptionInfo;
            ExceptionRecord*   rec = (ExceptionRecord*)ep->ExceptionRecord;
            if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;

            ulong faultAddr = rec->Info1;
            bool  isWrite   = rec->Info0 == 1;

            Range range;
            lock (s_lock)
            {
                int idx = -1;
                for (int i = 0; i < s_ranges.Count; i++)
                {
                    if (faultAddr >= s_ranges[i].HostStart && faultAddr < s_ranges[i].HostEnd)
                    {
                        idx = i; break;
                    }
                }
                if (idx < 0)
                {
                    // Unmapped MMIO — treat as "no device": reads return 0, writes
                    // discarded. Lets the kernel probe address space safely.
                    byte* ctxNoDev = (byte*)ep->ContextRecord;
                    byte* ripNoDev = (byte*)(*(ulong*)(ctxNoDev + 0xF8));
                    if (!Decode(ripNoDev, out var dNoDev)) return EXCEPTION_CONTINUE_SEARCH;
                    if (!isWrite)
                    {
                        if (dNoDev.SignExtend || dNoDev.ZeroExtendToFull)
                            WriteGpr(ctxNoDev, dNoDev.RegIndex, 0);
                        else
                        {
                            ulong prev = ReadGpr(ctxNoDev, dNoDev.RegIndex);
                            ulong mask = dNoDev.Width == 1 ? 0xFFUL : 0xFFFFUL;
                            WriteGpr(ctxNoDev, dNoDev.RegIndex, prev & ~mask);
                        }
                    }
                    *(ulong*)(ctxNoDev + 0xF8) = (ulong)(ripNoDev + dNoDev.Length);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                range = s_ranges[idx];
            }

            byte* context = (byte*)ep->ContextRecord;
            ulong rip64   = *(ulong*)(context + 0xF8);
            byte* rip     = (byte*)rip64;

            if (!Decode(rip, out var d)) return EXCEPTION_CONTINUE_SEARCH;

            uint offset = (uint)(faultAddr - range.HostStart);

            if (isWrite)
            {
                ulong srcVal = ReadGpr(context, d.RegIndex);
                uint  val    = d.Width switch
                {
                    1 => (uint)(byte)srcVal,
                    2 => (uint)(ushort)srcVal,
                    4 => (uint)srcVal,
                    _ => 0u,
                };
                range.Owner.Write(offset, d.Width, val);
            }
            else
            {
                uint val = range.Owner.Read(offset, d.Width);
                ulong wide;
                if (d.SignExtend)
                {
                    int sext = d.Width switch
                    {
                        1 => (int)(sbyte)val,
                        2 => (int)(short)val,
                        _ => (int)val,
                    };
                    wide = (uint)sext;
                }
                else if (d.ZeroExtendToFull)
                {
                    wide = val & 0xFFFFFFFFUL;
                }
                else
                {
                    ulong prev = ReadGpr(context, d.RegIndex);
                    ulong mask = d.Width == 1 ? 0xFFUL : 0xFFFFUL;
                    wide = (prev & ~mask) | (val & mask);
                }
                WriteGpr(context, d.RegIndex, wide);
            }

            *(ulong*)(context + 0xF8) = (ulong)(rip + d.Length);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        private static ulong ReadGpr(byte* ctx, int idx)            => *(ulong*)(ctx + 0x78 + idx * 8);
        private static void  WriteGpr(byte* ctx, int idx, ulong v)  => *(ulong*)(ctx + 0x78 + idx * 8) = v;

        // ── x86-64 MOV decoder (subset clang emits for memcpy<1/2/4>) ─────────

        private struct Decoded
        {
            public int  Length;
            public int  Width;
            public int  RegIndex;
            public bool SignExtend;
            public bool ZeroExtendToFull;
        }

        private static bool Decode(byte* p, out Decoded d)
        {
            d = default;
            int  pos    = 0;
            byte rex    = 0;
            bool size16 = false;

            while (true)
            {
                byte b = p[pos];
                if (b == 0x66) { size16 = true; pos++; continue; }
                if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) { pos++; continue; }
                if ((b & 0xF0) == 0x40) { rex = b; pos++; continue; }
                break;
            }

            byte op = p[pos++];
            bool twoByte = false;
            if (op == 0x0F) { twoByte = true; op = p[pos++]; }

            int  width;
            bool signExt    = false;
            bool zeroToFull = false;

            if (!twoByte)
            {
                switch (op)
                {
                    case 0x88: width = 1;                break;
                    case 0x89: width = size16 ? 2 : 4;   break;
                    case 0x8A: width = 1;                break;
                    case 0x8B: width = size16 ? 2 : 4;   zeroToFull = (width == 4); break;
                    default:   return false;
                }
            }
            else
            {
                switch (op)
                {
                    case 0xB6: width = 1; zeroToFull = true; break;
                    case 0xB7: width = 2; zeroToFull = true; break;
                    case 0xBE: width = 1; signExt    = true; break;
                    case 0xBF: width = 2; signExt    = true; break;
                    default:   return false;
                }
            }

            byte modrm = p[pos++];
            int mod   = (modrm >> 6) & 0x3;
            int reg   = (modrm >> 3) & 0x7;
            int rm    = modrm & 0x7;
            if (mod == 3) return false;
            if ((rex & 0x4) != 0) reg |= 0x8;
            if (rm == 4)                   pos++;       // SIB
            if (mod == 0 && rm == 5)       pos += 4;    // RIP-relative
            else if (mod == 1)             pos += 1;
            else if (mod == 2)             pos += 4;

            d.Length           = pos;
            d.Width            = width;
            d.RegIndex         = reg;
            d.SignExtend       = signExt;
            d.ZeroExtendToFull = zeroToFull;
            return true;
        }

        // ── Win32 ─────────────────────────────────────────────────────────────

        [StructLayout(LayoutKind.Sequential)]
        private struct ExceptionPointers
        {
            public IntPtr ExceptionRecord;
            public IntPtr ContextRecord;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ExceptionRecord
        {
            public uint   ExceptionCode;
            public uint   ExceptionFlags;
            public IntPtr NestedRecord;
            public IntPtr ExceptionAddress;
            public uint   NumberParameters;
            public uint   _pad;
            public ulong  Info0;
            public ulong  Info1;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr AddVectoredExceptionHandler(uint first, IntPtr handler);
    }
}
