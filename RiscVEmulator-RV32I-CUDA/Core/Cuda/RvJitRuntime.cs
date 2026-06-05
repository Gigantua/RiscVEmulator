using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// Runtime GPU JIT: translate a specific guest's read-only code to CUDA C
    /// (<see cref="RvJit.TranslateBody"/>), nvcc-compile it into a standalone
    /// <c>rv32i_jit_guest.dll</c>, load that DLL, and expose a
    /// <see cref="StepAll"/> entry that launches the JIT'd kernel.
    ///
    /// ── How the JIT DLL shares the interpreter DLL's managed state ──
    /// The interpreter DLL (<c>rv32i_cuda.dll</c>) owns the per-core
    /// <c>CoreState[]</c> / <c>CoreMem[]</c> arrays, both
    /// <c>cudaMallocManaged</c>. The JIT DLL is a SEPARATE module compiled at
    /// runtime — but both DLLs live in the same process and therefore share the
    /// one CUDA primary context for the device. A managed pointer minted by one
    /// module is fully valid in a kernel launched by the other (managed memory
    /// is context/process-global, not per-module). So the integration is:
    ///   1. the interpreter DLL exposes the array base pointers via
    ///      <c>cuda_rv32i_state_ptr()</c> / <c>cuda_rv32i_mem_ptr()</c>;
    ///   2. the JIT DLL's <c>cuda_jit_init(state, mem, ncores)</c> stashes
    ///      those exact pointers and never allocates its own state;
    ///   3. <c>cuda_jit_step_all(budget)</c> launches <c>jit_run_kernel</c>
    ///      over those arrays.
    /// Because the JIT and interpreter <c>.cu</c> both <c>#include
    /// rv32i_jit_shared.cuh</c>, the struct layouts and memory semantics are
    /// byte-identical, so the host can launch either kernel between peripheral-
    /// reconcile passes interchangeably and Doom's framebuffer / exit / traps
    /// all behave exactly as under the interpreter.
    /// </summary>
    public sealed class RvJitRuntime : IJitBackend
    {
        private IntPtr _lib;
        private StepAllDelegate? _stepAll;
        private bool _disposed;

        public string CuPath  { get; private set; } = "";
        public string DllPath { get; private set; } = "";
        public string Name => Path.GetFileName(DllPath);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InitDelegate(IntPtr state, IntPtr mem, int ncores);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int StepAllDelegate(long budget);

        /// <summary>
        /// Build the JIT DLL for the given guest. <paramref name="codeImg"/> is
        /// the raw bytes at guest VA <paramref name="lo"/> (the RO code span
        /// from <see cref="ElfLoader.ReadOnlyCodeSpan"/>); <paramref name="block"/>
        /// is the cores-per-block packing knob (1 for single-core latency).
        /// Throws on nvcc failure — the caller is expected to fall back to the
        /// interpreter.
        /// </summary>
        public static RvJitRuntime Build(byte[] codeImg, uint lo, uint hi, string buildDir, int block = 1)
        {
            Directory.CreateDirectory(buildDir);
            // SLICED translation across MANY .cu files so the single-threaded
            // device compiler (cicc) runs in parallel across all cores — one
            // giant .cu pins one core for many minutes on a guest like Doom.
            var (slices, dispatch, nslices) = RvJit.TranslateSlicedParts(codeImg, lo, hi);

            // main.cu: shared header + extern slice decls + dispatch + kernel + ABI.
            var ext = new StringBuilder();
            for (uint s = 0; s < nslices; s++)
                ext.Append($"__device__ uint32_t jit_slice_{s}(uint32_t*, uint32_t, Hart&, CoreMem&, long long*);\n");
            string mainCu = BuildCu(ext.ToString() + dispatch, lo, hi, block);

            // Spread slice functions over ~1 file/core for parallel device-compile
            // (cap at core count so we don't spawn dozens of nvcc/cl/cicc and spike RAM).
            int nParts = Math.Max(1, Math.Min(slices.Count, Environment.ProcessorCount));
            int per = (slices.Count + nParts - 1) / nParts;
            var parts = new List<string>();
            for (int i = 0; i < slices.Count; i += per)
            {
                // RVJIT_MEM_ONLY: skip the cold trap unit + cpu_step interpreter
                // in the shared header — slices only need the memory model +
                // jit_l*/jit_s*/jit_div* helpers, so cicc doesn't recompile the
                // giant interpreter switch in every one of these part files.
                var sb = new StringBuilder("#define RVJIT_MEM_ONLY\n#include \"rv32i_jit_shared.cuh\"\n");
                for (int j = i; j < Math.Min(i + per, slices.Count); j++) sb.Append(slices[j]);
                parts.Add(sb.ToString());
            }

            // Cache by content hash over every generated file.
            uint tag = Fnv1a(mainCu);
            foreach (var p in parts) tag = (tag ^ Fnv1a(p)) * 16777619u;
            string baseName = $"rv32i_jit_guest_{tag:X8}";
            string dllPath  = Path.Combine(buildDir, baseName + ".dll");
            string mainPath = Path.Combine(buildDir, baseName + "_main.cu");

            if (!File.Exists(dllPath))
            {
                var cuFiles = new List<string> { mainPath };
                File.WriteAllText(mainPath, mainCu);
                for (int i = 0; i < parts.Count; i++)
                {
                    string p = Path.Combine(buildDir, baseName + $"_p{i}.cu");
                    File.WriteAllText(p, parts[i]);
                    cuFiles.Add(p);
                }
                CompileParallel(cuFiles, dllPath, buildDir);
            }

            var rt = new RvJitRuntime { CuPath = mainPath, DllPath = dllPath };
            rt.LoadAndBind();
            return rt;
        }

        private void LoadAndBind()
        {
            _lib = NativeLibrary.Load(DllPath);

            // Wire the JIT DLL to the interpreter DLL's managed arrays. We pull
            // the base pointers from the interpreter (already initialised by
            // CudaEmulator's ctor) and hand them to the JIT module.
            IntPtr state  = Interp.cuda_rv32i_state_ptr();
            IntPtr mem    = Interp.cuda_rv32i_mem_ptr();
            int    ncores = Interp.cuda_rv32i_ncores();

            var init = Marshal.GetDelegateForFunctionPointer<InitDelegate>(
                NativeLibrary.GetExport(_lib, "cuda_jit_init"));
            int rc = init(state, mem, ncores);
            if (rc != 0)
                throw new InvalidOperationException($"cuda_jit_init failed (CUDA error {rc})");

            _stepAll = Marshal.GetDelegateForFunctionPointer<StepAllDelegate>(
                NativeLibrary.GetExport(_lib, "cuda_jit_step_all"));
        }

        /// <summary>Launch the JIT'd kernel for a budget of guest instructions
        /// across all cores. Returns a CUDA error code (0 = success).</summary>
        public int StepAll(long budget) => _stepAll!(budget);

        public void Dispose()
        {
            if (_disposed) return;
            if (_lib != IntPtr.Zero) { NativeLibrary.Free(_lib); _lib = IntPtr.Zero; }
            _disposed = true;
        }

        // ── Parallel nvcc driver ──────────────────────────────────────────
        // cicc (the device compiler) is single-threaded, so one big .cu pins one
        // core. We device-compile each .cu to a relocatable object (-dc -rdc=true)
        // in PARALLEL (one nvcc process per file, all cores busy), then device-
        // link + host-link them into the shared DLL. vcvars is captured ONCE and
        // applied to every child. Low device-opt (-O1, -Xcicc/-Xptxas -O1): the
        // translation IS the optimization, and -O3 makes cicc spend 1000+ CPU-s.
        private static void CompileParallel(List<string> cuFiles, string dllPath, string buildDir)
        {
            string vc        = @"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat";
            string nativeDir = NativeIncludeDir();

            Process StartBat(string bat)
            {
                var psi = new ProcessStartInfo("cmd.exe") { UseShellExecute = false, CreateNoWindow = true };
                psi.ArgumentList.Add("/c");
                psi.ArgumentList.Add(bat);
                return Process.Start(psi)!;
            }

            // 1) parallel device-compile each .cu → .obj. Each file gets its own
            //    .bat that calls vcvars itself (reliably puts cl.exe in PATH —
            //    propagating a captured env to the child did not work), then nvcc
            //    -dc (relocatable device code). All launched at once → all cores.
            var objs  = new List<string>();
            var procs = new List<(Process p, string log, string cu)>();
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
                    // --disable-optimizer-constants: the big dispatch/jump tables
                    // otherwise overflow ptxas's compiler-generated constant bank
                    // (nvlink "uses too much data for compiler-generated constants").
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
            if (errors.Length > 0) throw new InvalidOperationException("JIT device-compile failed:\n" + errors);

            // 2) device-link + host-link → shared DLL
            string lbat = Path.Combine(buildDir, "jit_link.bat");
            string llog = Path.Combine(buildDir, "jit_link.log");
            string objArgs = string.Join(" ", objs.ConvertAll(o => $"\"{o}\""));
            File.WriteAllText(lbat,
                "@echo off\r\n" +
                $"call \"{vc}\" >nul 2>&1\r\n" +
                $"nvcc -shared -rdc=true -cudart static -arch=sm_86 -w -o \"{dllPath}\" {objArgs} > \"{llog}\" 2>&1\r\n");
            var lp = StartBat(lbat);
            lp.WaitForExit();
            if (lp.ExitCode != 0 || !File.Exists(dllPath))
                throw new InvalidOperationException("JIT link failed:\n" + (File.Exists(llog) ? File.ReadAllText(llog) : ""));
        }

        // Locate Native/ (holds rv32i_jit_shared.cuh) relative to the running app.
        private static string NativeIncludeDir()
        {
            string? root = AppContext.BaseDirectory;
            while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
                root = Path.GetDirectoryName(root);
            if (root == null)
                throw new DirectoryNotFoundException("RiscVEmulator.sln not found (needed to locate Native/rv32i_jit_shared.cuh).");
            return Path.Combine(root, "Native");
        }

        private static uint Fnv1a(string s)
        {
            uint h = 2166136261u;
            foreach (char c in s) { h ^= c; h *= 16777619u; }
            return h;
        }

        // ── The generated .cu: shared header + the register-resident kernel +
        //    the per-guest translated body + the C-ABI the runtime binds to. ──
        private static string BuildCu(string body, uint lo, uint hi, int block)
        {
            return $$"""
// AUTO-GENERATED by Core/Cuda/RvJitRuntime.cs — do not edit by hand.
// Per-guest GPU JIT module. Shares rv32i_cuda.dll's managed CoreState[]/
// CoreMem[] (passed in via cuda_jit_init); #includes the same shared header so
// struct layout + memory semantics are byte-identical to the interpreter.
#include "rv32i_jit_shared.cuh"

static constexpr int      JIT_BLOCK = {{block}};
// Translated guest code range. jit_dispatch routes a pc here to its slice
// function; outside this range (or for SYSTEM/illegal) the kernel falls back to
// the interpreter.
static constexpr uint32_t JIT_LO = 0x{{lo:X}}u;
static constexpr uint32_t JIT_HI = 0x{{hi:X}}u;

// ── translated guest: per-slice __device__ functions + jit_dispatch(pc) ──
// Guest regs are a per-thread local array passed as uint32_t* R. Branches
// inside a slice chain via goto; cross-slice/JALR transfers return the next pc;
// SYSTEM/illegal set h.pc and return 0xFFFFFFFE (interpreter fallback).
{{body}}

// One core per thread. The dispatch loop calls slice functions until a pc
// leaves the translated range / needs the interpreter, services that via the
// interp seam (which also handles interrupts + the trap-return gateway), and
// resumes. Hang-safe: in-slice back-edges decrement *bl, the loop decrements
// once per dispatch, and the interp seam decrements per step.
__global__ void __launch_bounds__({{(block <= 32 ? 32 : block <= 128 ? 128 : 256)}})
jit_run_kernel(CoreState* st, CoreMem* mm, int ncores, long long budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    CoreMem&   m = mm[id];

    constexpr int STRIDE = 33;            // shared regfile slice for the interp seam
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
            // interp seam: run the interpreter until control returns to
            // translated, aligned code (or halt / budget) — also services
            // interrupts and the trap-return gateway.
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
        bl--;                              // bound cross-slice loops
        if (nxt == 0xFFFFFFFEu) {
            // SYSTEM / illegal at h.pc (set by the slice): interpret one step.
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

// ════════════════════════════════════════════════════════════════════
// Host C-ABI — shares rv32i_cuda.dll's managed arrays (see RvJitRuntime.cs).
// ════════════════════════════════════════════════════════════════════
#define JAPI extern "C" __declspec(dllexport)

static CoreState* gj_state  = nullptr;
static CoreMem*   gj_mem    = nullptr;
static int        gj_ncores = 0;

// Take the interpreter DLL's managed base pointers. We do NOT allocate or free
// them — rv32i_cuda.dll owns their lifetime (cuda_rv32i_shutdown frees them).
JAPI int cuda_jit_init(void* state, void* mem, int ncores) {
    gj_state  = (CoreState*)state;
    gj_mem    = (CoreMem*)mem;
    gj_ncores = ncores;
    return (int)cudaGetLastError();
}

JAPI int cuda_jit_step_all(long long budget) {
    if (gj_ncores <= 0) return 0;
    int block = JIT_BLOCK;
    if (block > gj_ncores) block = gj_ncores;
    if (block < 1) block = 1;
    int    grid  = (gj_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 33 * sizeof(uint32_t);
    jit_run_kernel<<<grid, block, shmem>>>(gj_state, gj_mem, gj_ncores, budget);
    cudaError_t le = cudaGetLastError();
    cudaError_t se = cudaDeviceSynchronize();
    return le != cudaSuccess ? (int)le : (int)se;
}
""";
        }

        // ── P/Invoke into the interpreter DLL for the shared base pointers. ──
        private static class Interp
        {
            private const string Lib = "rv32i_cuda";
            [DllImport(Lib)] public static extern IntPtr cuda_rv32i_state_ptr();
            [DllImport(Lib)] public static extern IntPtr cuda_rv32i_mem_ptr();
            [DllImport(Lib)] public static extern int    cuda_rv32i_ncores();
        }
    }
}
