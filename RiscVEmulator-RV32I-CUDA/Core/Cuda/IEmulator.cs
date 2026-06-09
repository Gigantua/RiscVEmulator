namespace RiscVEmulator.Core
{
    public interface IEmulator
    {
        int  StepN(int n);
        bool IsHalted { get; }
        int  ExitCode { get; }
        void SetHalted(bool value);
        Action<char>? OutputHandler { get; set; }
    }
}
