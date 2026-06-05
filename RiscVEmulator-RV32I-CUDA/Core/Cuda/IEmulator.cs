namespace RiscVEmulator.Core
{
    /// <summary>
    /// Minimal driver seam shared by the CPU <see cref="Emulator"/> and the
    /// GPU <see cref="Cuda.CudaEmulator"/> so a frontend (e.g. SdlWindow) can
    /// drive either backend. Both expose the same step/halt/exit surface.
    /// </summary>
    public interface IEmulator
    {
        /// <summary>Run up to <paramref name="n"/> guest instructions; returns steps issued.</summary>
        int  StepN(int n);
        bool IsHalted { get; }
        int  ExitCode { get; }
        uint PC       { get; }
        void SetHalted(bool value);
        Action<char>? OutputHandler { get; set; }
    }
}
