namespace RiscVEmulator.Core.Peripherals;

/// <summary>SYSCON power/reboot peripheral. Guarded.</summary>
public sealed class SysconDevice : IPeripheral
{
    public uint BaseAddress { get; }
    public uint Size        => 0x1000u;
    public bool IsGuarded   => true;

    public SysconDevice(uint baseAddress = 0x11100000u) { BaseAddress = baseAddress; }

    public Action? OnPowerOff { get; set; }
    public Action? OnReboot   { get; set; }

    public uint Read(uint offset, int width) => 0;

    public void Write(uint offset, int width, uint value)
    {
        if (offset != 0) return;
        if      (value == 0x5555u) OnPowerOff?.Invoke();
        else if (value == 0x7777u) OnReboot?.Invoke();
    }
}
