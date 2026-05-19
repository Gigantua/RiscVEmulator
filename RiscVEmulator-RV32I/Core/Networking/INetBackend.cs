using System;

namespace RiscVEmulator.Core.Networking
{
    /// <summary>
    /// Pluggable host-side packet path for <c>VirtioNetDevice</c>.
    ///
    /// <para>Guest sends packets via <see cref="SendFromGuest"/>. The backend either
    /// forwards them to the real network (libslirp / TAP / raw socket) or routes them
    /// internally. When packets arrive from the host side, the backend invokes
    /// <see cref="ReceivedFromHost"/>, which the device hooks to push them into the
    /// RX virtqueue.</para>
    ///
    /// <para>Packet format on both directions is bare Ethernet frames (no virtio_net_hdr;
    /// the device strips / prepends that 12-byte header itself).</para>
    /// </summary>
    public interface INetBackend : IDisposable
    {
        /// <summary>Invoked by the device when the guest transmits a frame.</summary>
        void SendFromGuest(ReadOnlySpan<byte> frame);

        /// <summary>Hooked by the device. Backend calls this when a frame arrives
        /// from the host side and should be delivered to the guest.</summary>
        Action<byte[]>? ReceivedFromHost { get; set; }

        /// <summary>MAC address the device advertises to the guest. The backend
        /// may return any 6 bytes — typically locally-administered (first byte's
        /// bit 1 set, bit 0 clear: 0x02-0xFE for the first octet).</summary>
        byte[] GuestMac { get; }
    }

    /// <summary>Default backend: discards TX, delivers no RX. Lets the guest's
    /// virtio driver come up without errors but provides no actual connectivity.
    /// Replace with a libslirp- or Win32-backed implementation for real networking.</summary>
    public sealed class NullNetBackend : INetBackend
    {
        public byte[] GuestMac { get; } = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
        public Action<byte[]>? ReceivedFromHost { get; set; }
        public void SendFromGuest(ReadOnlySpan<byte> frame) { /* drop */ }
        public void Dispose() { }
    }
}
