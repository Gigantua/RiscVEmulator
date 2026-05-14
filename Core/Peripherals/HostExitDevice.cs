namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>Host-exit at 0x40000000. Guarded. Bare-metal _exit writes the
    /// exit code here; the VEH dispatches and we halt.</summary>
    public sealed class HostExitDevice : IPeripheral
    {
        public uint BaseAddress => 0x40000000;
        public uint Size        => 0x10;
        public bool IsGuarded   => true;

        public Action<int>? OnExit { get; set; }

        public uint Read(uint offset, int width) => 0;

        public void Write(uint offset, int width, uint value)
        {
            if (offset == 0) OnExit?.Invoke((int)value);
        }
    }
}
