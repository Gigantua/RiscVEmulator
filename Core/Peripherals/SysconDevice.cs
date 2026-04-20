namespace RiscVEmulator.Core.Peripherals;

/// <summary>
/// SYSCON power/reboot controller at 0x11100000 (compatible with mini-rv32ima Linux images).
/// Write 0x5555 to offset 0 to power off; write 0x7777 to reboot.
/// </summary>
public sealed class SysconDevice : IPeripheral
{
    public uint BaseAddress => 0x11100000u;
    public uint Size        => 0x1000u;

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
