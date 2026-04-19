namespace RiscVEmulator.Core
{
    /// <summary>
    /// 32 general-purpose RISC-V registers (x0–x31).
    /// x0 is hardwired to zero: reads always return 0, writes are silently discarded.
    /// </summary>
    public class RegisterFile
    {
        private readonly uint[] _regs = new uint[32];

        public uint Read(int index)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(index, 31);
            return _regs[index]; // _regs[0] is always 0
        }

        public void Write(int index, uint value)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(index, 31);
            if (index != 0) // x0 is hardwired zero
                _regs[index] = value;
        }
    }
}
