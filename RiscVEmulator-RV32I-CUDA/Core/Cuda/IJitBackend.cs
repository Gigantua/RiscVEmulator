using System;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// A per-guest GPU JIT backend that runs the translated kernel over the
    /// interpreter DLL's shared managed CoreState[]/CoreMem[]. Two implementations:
    ///   • <see cref="RvJitRuntime"/> — CUDA-C → nvcc → DLL, loaded with NativeLibrary;
    ///   • <see cref="PtxJitRuntime"/> — CUDA-C → cubin, loaded + launched in-process
    ///     via the CUDA Driver API (no DLL).
    /// Both are interchangeable per launch with the interpreter kernel, so the host
    /// peripheral reconcile (StageInputs/DrainOutputs) is identical either way.
    /// </summary>
    public interface IJitBackend : IDisposable
    {
        /// <summary>Launch the JIT'd kernel for a budget of guest steps across all
        /// cores. Returns a CUDA error code (0 = success).</summary>
        int StepAll(long budget);

        /// <summary>Short human-readable name (artifact filename) for logging.</summary>
        string Name { get; }
    }
}
