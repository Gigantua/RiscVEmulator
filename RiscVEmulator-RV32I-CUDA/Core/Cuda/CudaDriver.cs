using System;
using System.Runtime.InteropServices;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// Minimal P/Invoke surface for the CUDA Driver API (nvcuda.dll) — just what
    /// the in-process PTX/cubin JIT (<see cref="PtxJitRuntime"/>) needs to load a
    /// cubin and launch its kernel. On x64 there is a single calling convention,
    /// so the attribute is moot, but we name it for clarity.
    ///
    /// The driver-API context used is the device's PRIMARY context — the same one
    /// the CUDA runtime (cudart, inside rv32i_cuda.dll) lazily created for its
    /// cudaMallocManaged allocations. Retaining the primary context and making it
    /// current means the managed CoreState[]/CoreMem[] pointers are valid in
    /// kernels we launch through the driver API.
    /// </summary>
    internal static class CudaDriver
    {
        private const string Lib = "nvcuda";
        private const CallingConvention CC = CallingConvention.Cdecl;

        [DllImport(Lib, CallingConvention = CC)] public static extern int cuInit(uint flags);
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuDeviceGet(out int dev, int ordinal);
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuDevicePrimaryCtxRetain(out IntPtr ctx, int dev);
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuCtxSetCurrent(IntPtr ctx);
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuModuleLoadData(out IntPtr module, byte[] image);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        public static extern int cuModuleGetFunction(out IntPtr func, IntPtr module, string name);
        [DllImport(Lib, CallingConvention = CC)]
        public static extern int cuLaunchKernel(IntPtr f,
            uint gridX, uint gridY, uint gridZ,
            uint blockX, uint blockY, uint blockZ,
            uint sharedMemBytes, IntPtr hStream, IntPtr kernelParams, IntPtr extra);
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuCtxSynchronize();
        [DllImport(Lib, CallingConvention = CC)] public static extern int cuModuleUnload(IntPtr module);

        private static bool _inited;
        private static IntPtr _ctx;
        private static readonly object _lock = new();

        /// <summary>cuInit + retain & set the device's primary context (idempotent).
        /// Throws on failure.</summary>
        public static void EnsureContext()
        {
            lock (_lock)
            {
                if (_inited) return;
                Check(cuInit(0), "cuInit");
                Check(cuDeviceGet(out int dev, 0), "cuDeviceGet");
                Check(cuDevicePrimaryCtxRetain(out _ctx, dev), "cuDevicePrimaryCtxRetain");
                Check(cuCtxSetCurrent(_ctx), "cuCtxSetCurrent");
                _inited = true;
            }
        }

        public static void Check(int rc, string what)
        {
            if (rc != 0) throw new InvalidOperationException($"{what} failed (CUDA driver error {rc})");
        }
    }
}
