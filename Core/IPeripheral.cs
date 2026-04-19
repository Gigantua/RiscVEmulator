namespace RiscVEmulator.Core
{
    /// <summary>
    /// A memory-mapped I/O peripheral that occupies a contiguous address range.
    /// The MemoryBus dispatches reads/writes to peripherals based on address.
    /// </summary>
    public interface IPeripheral
    {
        /// <summary>Base address of this peripheral's MMIO region.</summary>
        uint BaseAddress { get; }

        /// <summary>Size in bytes of this peripheral's MMIO region.</summary>
        uint Size { get; }

        /// <summary>
        /// Read from a peripheral register.
        /// </summary>
        /// <param name="offset">Byte offset from BaseAddress.</param>
        /// <param name="width">Access width: 1, 2, or 4 bytes.</param>
        /// <returns>Value read (zero-extended to 32 bits).</returns>
        uint Read(uint offset, int width);

        /// <summary>
        /// Write to a peripheral register.
        /// </summary>
        /// <param name="offset">Byte offset from BaseAddress.</param>
        /// <param name="width">Access width: 1, 2, or 4 bytes.</param>
        /// <param name="value">Value to write (lower bits used based on width).</param>
        void Write(uint offset, int width, uint value);
    }
}
