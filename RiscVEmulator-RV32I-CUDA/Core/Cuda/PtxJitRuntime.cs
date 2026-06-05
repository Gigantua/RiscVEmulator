using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// In-process driver-API GPU JIT. Translates the guest's RO code to a single
    /// CUDA-C translation unit (full memory seam + interp fallback, identical to
    /// <see cref="RvJitRuntime"/>'s codegen so it runs real guests like DOOM),
    /// compiles it to a <b>cubin</b> with nvcc, and loads + launches it
    /// <b>in-process via the CUDA Driver API</b> (cuModuleLoadData / cuLaunchKernel)
    /// — no DLL, no NativeLibrary.Load, no host C-ABI shim.
    ///
    /// The kernel runs over the SAME managed CoreState[]/CoreMem[] the interpreter
    /// DLL allocated (pointers pulled via cuda_rv32i_state_ptr/mem_ptr); we launch
    /// it on the device's primary context (shared with cudart), so those managed
    /// pointers are valid. Per launch it is interchangeable with the interpreter
    /// kernel, so host peripheral reconcile is unchanged.
    /// </summary>
    public sealed class PtxJitRuntime : IJitBackend
    {
        private IntPtr _module, _func;
        private IntPtr _argv;                 // void** kernelParams (4 slots)
        private IntPtr _aState, _aMem, _aN, _aBudget;
        private int _grid, _block;
        private uint _shmem;
        private bool _disposed;
        public string CubinPath { get; private set; } = "";
        public string Name => Path.GetFileName(CubinPath);

        public static PtxJitRuntime Build(byte[] codeImg, uint lo, uint hi, string buildDir, int block = 1)
        {
            Directory.CreateDirectory(buildDir);
            // SLICED translation across many files → parallel nvcc -dc (cicc is
            // single-threaded; one giant TU pins one core for tens of minutes on a
            // guest like Doom). The relocatable objects are then device-linked into
            // a cubin with nvlink and loaded via the driver API.
            var (slices, dispatch, nslices) = RvJit.TranslateSlicedParts(codeImg, lo, hi);
            var ext = new StringBuilder();
            for (uint s = 0; s < nslices; s++)
                ext.Append($"__device__ uint32_t jit_slice_{s}(uint32_t*, uint32_t, Hart&, CoreMem&, long long*);\n");
            string mainCu = BuildMainCu(ext.ToString() + dispatch, lo, hi, block);

            int nParts = Math.Max(1, Math.Min(slices.Count, Environment.ProcessorCount));
            int per = (slices.Count + nParts - 1) / nParts;
            var parts = new System.Collections.Generic.List<string>();
            for (int i = 0; i < slices.Count; i += per)
            {
                var sb = new StringBuilder("#define RVJIT_MEM_ONLY\n#include \"rv32i_jit_shared.cuh\"\n");
                for (int j = i; j < Math.Min(i + per, slices.Count); j++) sb.Append(slices[j]);
                parts.Add(sb.ToString());
            }

            uint tag = Fnv1a(mainCu);
            foreach (var p in parts) tag = (tag ^ Fnv1a(p)) * 16777619u;
            string baseName  = $"rv32i_ptxjit_{tag:X8}";
            string cubinPath = Path.Combine(buildDir, baseName + ".cubin");
            string mainPath  = Path.Combine(buildDir, baseName + "_main.cu");

            if (!File.Exists(cubinPath))
            {
                var cuFiles = new System.Collections.Generic.List<string> { mainPath };
                File.WriteAllText(mainPath, mainCu);
                for (int i = 0; i < parts.Count; i++)
                {
                    string p = Path.Combine(buildDir, baseName + $"_p{i}.cu");
                    File.WriteAllText(p, parts[i]);
                    cuFiles.Add(p);
                }
                CompileCubin(cuFiles, cubinPath, buildDir);
            }

            var rt = new PtxJitRuntime { CubinPath = cubinPath };
            rt.LoadAndBind(block);
            return rt;
        }

        private void LoadAndBind(int block)
        {
            CudaDriver.EnsureContext();
            byte[] cubin = File.ReadAllBytes(CubinPath);
            CudaDriver.Check(CudaDriver.cuModuleLoadData(out _module, cubin), "cuModuleLoadData");
            CudaDriver.Check(CudaDriver.cuModuleGetFunction(out _func, _module, "jit_run_kernel"), "cuModuleGetFunction(jit_run_kernel)");

            IntPtr state = Interp.cuda_rv32i_state_ptr();
            IntPtr mem   = Interp.cuda_rv32i_mem_ptr();
            int    ncores = Interp.cuda_rv32i_ncores();
            _block = Math.Clamp(block, 1, 256);
            if (_block > ncores) _block = ncores;
            if (_block < 1) _block = 1;
            _grid  = (ncores + _block - 1) / _block;
            _shmem = (uint)(_block * 33 * sizeof(uint));   // shared regfile slice for the interp seam

            // Stable storage for the 4 kernel args + the void** pointer array.
            _aState  = Marshal.AllocHGlobal(IntPtr.Size); Marshal.WriteIntPtr(_aState, state);
            _aMem    = Marshal.AllocHGlobal(IntPtr.Size); Marshal.WriteIntPtr(_aMem, mem);
            _aN      = Marshal.AllocHGlobal(sizeof(int));  Marshal.WriteInt32(_aN, ncores);
            _aBudget = Marshal.AllocHGlobal(sizeof(long)); Marshal.WriteInt64(_aBudget, 0);
            _argv    = Marshal.AllocHGlobal(IntPtr.Size * 4);
            Marshal.WriteIntPtr(_argv, 0 * IntPtr.Size, _aState);
            Marshal.WriteIntPtr(_argv, 1 * IntPtr.Size, _aMem);
            Marshal.WriteIntPtr(_argv, 2 * IntPtr.Size, _aN);
            Marshal.WriteIntPtr(_argv, 3 * IntPtr.Size, _aBudget);
        }

        public int StepAll(long budget)
        {
            Marshal.WriteInt64(_aBudget, budget);
            int rc = CudaDriver.cuLaunchKernel(_func,
                (uint)_grid, 1, 1, (uint)_block, 1, 1,
                _shmem, IntPtr.Zero, _argv, IntPtr.Zero);
            if (rc != 0) return rc;
            return CudaDriver.cuCtxSynchronize();
        }

        public void Dispose()
        {
            if (_disposed) return;
            if (_argv    != IntPtr.Zero) Marshal.FreeHGlobal(_argv);
            if (_aState  != IntPtr.Zero) Marshal.FreeHGlobal(_aState);
            if (_aMem    != IntPtr.Zero) Marshal.FreeHGlobal(_aMem);
            if (_aN      != IntPtr.Zero) Marshal.FreeHGlobal(_aN);
            if (_aBudget != IntPtr.Zero) Marshal.FreeHGlobal(_aBudget);
            if (_module  != IntPtr.Zero) CudaDriver.cuModuleUnload(_module);
            _disposed = true;
        }

        // ── parallel nvcc -dc → objects, then nvlink → cubin (driver-API loadable). ──
        // cicc is single-threaded, so each .cu is device-compiled to a relocatable
        // object in its own process (all cores busy), then the device linker
        // (nvlink) combines them into ONE cubin — no host link, no DLL.
        private static void CompileCubin(System.Collections.Generic.List<string> cuFiles, string cubinPath, string buildDir)
        {
            string vc        = @"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat";
            string nativeDir = NativeIncludeDir();

            Process StartBat(string bat)
            {
                var psi = new ProcessStartInfo("cmd.exe") { UseShellExecute = false, CreateNoWindow = true };
                psi.ArgumentList.Add("/c"); psi.ArgumentList.Add(bat);
                return Process.Start(psi)!;
            }

            // 1) parallel device-compile each .cu → .obj (relocatable device code).
            var objs  = new System.Collections.Generic.List<string>();
            var procs = new System.Collections.Generic.List<(Process p, string log, string cu)>();
            foreach (var cu in cuFiles)
            {
                string obj = Path.ChangeExtension(cu, ".obj");
                string bat = Path.ChangeExtension(cu, ".bat");
                string log = Path.ChangeExtension(cu, ".log");
                objs.Add(obj);
                File.WriteAllText(bat,
                    "@echo off\r\n" +
                    $"call \"{vc}\" >nul 2>&1\r\n" +
                    $"nvcc -O1 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 -w -rdc=true " +
                    $"-Xcicc -O1 -Xptxas -O1 -Xptxas --disable-optimizer-constants " +
                    $"-dc -I\"{nativeDir}\" -o \"{obj}\" \"{cu}\" > \"{log}\" 2>&1\r\n");
                procs.Add((StartBat(bat), log, cu));
            }
            var errors = new StringBuilder();
            foreach (var (p, log, cu) in procs)
            {
                p.WaitForExit();
                if (p.ExitCode != 0)
                    errors.AppendLine($"[{Path.GetFileName(cu)}]\n{(File.Exists(log) ? File.ReadAllText(log) : "")}");
            }
            if (errors.Length > 0) throw new InvalidOperationException("PTX-JIT device-compile failed:\n" + errors);

            // 2) device-link the objects into a single cubin (nvlink, not a DLL).
            string lbat = Path.Combine(buildDir, "ptxjit_link.bat");
            string llog = Path.Combine(buildDir, "ptxjit_link.log");
            string objArgs = string.Join(" ", objs.ConvertAll(o => $"\"{o}\""));
            File.WriteAllText(lbat,
                "@echo off\r\n" +
                $"call \"{vc}\" >nul 2>&1\r\n" +
                $"nvlink -arch=sm_86 {objArgs} -o \"{cubinPath}\" > \"{llog}\" 2>&1\r\n");
            var lp = StartBat(lbat); lp.WaitForExit();
            if (lp.ExitCode != 0 || !File.Exists(cubinPath))
                throw new InvalidOperationException("PTX-JIT nvlink failed:\n" + (File.Exists(llog) ? File.ReadAllText(llog) : ""));
        }

        private static string NativeIncludeDir()
        {
            string? root = AppContext.BaseDirectory;
            while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
                root = Path.GetDirectoryName(root);
            if (root == null)
                throw new DirectoryNotFoundException("RiscVEmulator.sln not found (needed for Native/rv32i_jit_shared.cuh).");
            return Path.Combine(root, "Native");
        }

        private static uint Fnv1a(string s)
        {
            uint h = 2166136261u;
            foreach (char c in s) { h ^= c; h *= 16777619u; }
            return h;
        }

        // ── The single device-only TU: shared header + inlined slices + dispatch +
        //    the register-resident kernel (extern "C" so cuModuleGetFunction finds
        //    it by plain name). No host C-ABI — the driver API launches it directly. ──
        private static string BuildMainCu(string body, uint lo, uint hi, int block)
        {
            int mb = block <= 32 ? 32 : block <= 128 ? 128 : 256;

            return $$"""
// AUTO-GENERATED by Core/Cuda/PtxJitRuntime.cs — per-guest GPU JIT (cubin, driver-API loaded).
#include "rv32i_jit_shared.cuh"

static constexpr int      JIT_BLOCK = {{block}};
static constexpr uint32_t JIT_LO = 0x{{lo:X}}u;
static constexpr uint32_t JIT_HI = 0x{{hi:X}}u;

// extern slice decls (defined in the parallel-compiled _pN.cu) + jit_dispatch(pc)
{{body}}

extern "C" __global__ void __launch_bounds__({{mb}})
jit_run_kernel(CoreState* st, CoreMem* mm, int ncores, long long budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    CoreMem&   m = mm[id];

    constexpr int STRIDE = 33;
    extern __shared__ uint32_t s_state[];
    Hart h;
    h.regs     = &s_state[threadIdx.x * STRIDE];
    h.sec      = nullptr;
    h.sbase[0] = 0xFFFFFFFFu; h.sbase[1] = 0xFFFFFFFFu; h.pend = 0;
    h.g        = &g;
    h.pc = g.pc; h.priv = g.priv; h.pending = g.pending;
    h.halted = g.halted; h.exitcode = g.exitcode; h.exited = g.exited;

    uint32_t R[32];
    #pragma unroll
    for (int i = 0; i < 32; i++) R[i] = g.regs[i];
    uint32_t  pc = g.pc;
    long long bl = budget;

    while (!h.halted && bl > 0) {
        bool inJit = (pc - JIT_LO < JIT_HI - JIT_LO) && ((pc & 3u) == 0);
        if (!inJit || h.pending || pc == PV_RESUME_GATEWAY) {
            #pragma unroll
            for (int i = 0; i < 32; i++) h.regs[i] = R[i];
            h.pc = pc;
            while (!h.halted && bl > 0) {
                bool ok = (h.pc - JIT_LO < JIT_HI - JIT_LO) && ((h.pc & 3u) == 0)
                          && !h.pending && h.pc != PV_RESUME_GATEWAY;
                if (ok) break;
                jit_interp_step(h, m);
                bl--;
            }
            #pragma unroll
            for (int i = 0; i < 32; i++) R[i] = h.regs[i];
            pc = h.pc;
            continue;
        }
        uint32_t nxt = jit_dispatch(R, pc, h, m, &bl);
        bl--;
        if (nxt == 0xFFFFFFFEu) {
            #pragma unroll
            for (int i = 0; i < 32; i++) h.regs[i] = R[i];
            jit_interp_step(h, m);
            bl--;
            #pragma unroll
            for (int i = 0; i < 32; i++) R[i] = h.regs[i];
            pc = h.pc;
        } else {
            pc = nxt;
        }
    }

    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = R[i];
    g.pc = pc; g.priv = h.priv; g.pending = h.pending;
    g.halted = h.halted; g.exitcode = h.exitcode; g.exited = h.exited;
}
""";
        }

        private static class Interp
        {
            private const string Lib = "rv32i_cuda";
            [DllImport(Lib)] public static extern IntPtr cuda_rv32i_state_ptr();
            [DllImport(Lib)] public static extern IntPtr cuda_rv32i_mem_ptr();
            [DllImport(Lib)] public static extern int    cuda_rv32i_ncores();
        }
    }
}
