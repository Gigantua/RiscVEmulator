namespace RiscVEmulator.Core
{
    /// <summary>
    /// A memory-mapped peripheral. Lives in a slice of the host memory
    /// reservation at <see cref="BaseAddress"/>. Plain peripherals back
    /// themselves with PAGE_READWRITE pages (the CPU dereferences them
    /// directly, no callback). Guarded peripherals back themselves with
    /// PAGE_NOACCESS — every access AVs and the host's VEH calls
    /// <see cref="Read"/> / <see cref="Write"/>.
    /// </summary>
    public interface IPeripheral
    {
        uint BaseAddress { get; }
        uint Size        { get; }
        bool IsGuarded   { get; }

        /// <summary>
        /// Called once by the Emulator after committing this peripheral's slice
        /// in the reservation. Plain peripherals can stash the pointer to access
        /// their bytes directly. Guarded peripherals can ignore it.
        /// </summary>
        unsafe void Bind(byte* slice) { }

        /// <summary>Read from a guarded peripheral. Plain peripherals don't see this.</summary>
        uint Read(uint offset, int width);

        /// <summary>Write to a guarded peripheral. Plain peripherals don't see this.</summary>
        void Write(uint offset, int width, uint value);
    }
}
