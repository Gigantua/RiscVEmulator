using System;
using System.Runtime.CompilerServices;
using System.Threading;
using RiscVEmulator.Core.Networking;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// virtio-mmio v2 net device. Two split virtqueues:
    ///   Queue 0 = RX (host → guest)
    ///   Queue 1 = TX (guest → host)
    /// Frames cross the boundary via <see cref="INetBackend"/>; the device
    /// strips/prepends the 12-byte virtio_net_hdr.
    ///
    /// Spec: virtio-v1.1 §4.2 (MMIO transport) and §5.1 (network device).
    /// </summary>
    public sealed unsafe class VirtioNetDevice : IPeripheral, IDisposable
    {
        // ── Constants ─────────────────────────────────────────────────────────

        private const uint MAGIC      = 0x74726976u;  // 'virt'
        private const uint VERSION    = 2u;           // modern
        private const uint DEVICE_ID  = 1u;           // network
        private const uint VENDOR_ID  = 0x554D4551u;  // 'QEMU' — convention

        // Status register bits.
        private const uint STATUS_ACKNOWLEDGE        = 1;
        private const uint STATUS_DRIVER             = 2;
        private const uint STATUS_DRIVER_OK          = 4;
        private const uint STATUS_FEATURES_OK        = 8;
        private const uint STATUS_DEVICE_NEEDS_RESET = 64;
        private const uint STATUS_FAILED             = 128;

        // Feature bits we advertise.
        private const int VIRTIO_NET_F_MAC      = 5;
        private const int VIRTIO_F_VERSION_1    = 32;

        // Descriptor flags.
        private const ushort VRING_DESC_F_NEXT  = 1;
        private const ushort VRING_DESC_F_WRITE = 2;

        private const int VirtioNetHdrLen = 12;
        private const int NumQueues       = 2;
        private const int MaxQueueSize    = 256;

        // ── Wiring ────────────────────────────────────────────────────────────

        public uint BaseAddress { get; }
        public uint Size        => 0x1000;
        public bool IsGuarded   => true;

        private readonly PlicDevice _plic;
        private readonly int        _irqNum;
        private readonly INetBackend _backend;
        private readonly byte*      _ramBase;   // host pointer to guest paddr 0

        // ── Device state ──────────────────────────────────────────────────────

        private uint _deviceFeaturesSel;
        private uint _driverFeaturesSel;
        private ulong _driverFeatures;  // 64-bit accumulated across sel writes
        private uint _status;
        private uint _interruptStatus;

        // Per-queue selection state.
        private uint _queueSel;

        // Per-queue parameters set by the driver.
        private readonly uint[]  _queueNum   = new uint[NumQueues];
        private readonly uint[]  _queueReady = new uint[NumQueues];
        private readonly ulong[] _descAddr   = new ulong[NumQueues];
        private readonly ulong[] _driverAddr = new ulong[NumQueues];  // available ring
        private readonly ulong[] _deviceAddr = new ulong[NumQueues];  // used ring

        // Last-seen indices.
        private readonly ushort[] _lastAvailIdx = new ushort[NumQueues];
        private readonly ushort[] _lastUsedIdx  = new ushort[NumQueues];

        // ── Construction ──────────────────────────────────────────────────────

        public VirtioNetDevice(uint baseAddress, PlicDevice plic, int irqNum,
                               INetBackend backend, IntPtr ramBase)
        {
            BaseAddress = baseAddress;
            _plic       = plic;
            _irqNum     = irqNum;
            _backend    = backend;
            _ramBase    = (byte*)ramBase;
            _backend.ReceivedFromHost = OnHostFrame;
        }

        // ── MMIO Read ─────────────────────────────────────────────────────────

        public uint Read(uint offset, int width)
        {
            switch (offset)
            {
                case 0x000: return MAGIC;
                case 0x004: return VERSION;
                case 0x008: return DEVICE_ID;
                case 0x00C: return VENDOR_ID;
                case 0x010: return DeviceFeaturesWindow(_deviceFeaturesSel);
                case 0x034: return MaxQueueSize;
                case 0x044: return _queueSel < NumQueues ? _queueReady[_queueSel] : 0;
                case 0x060: return _interruptStatus;
                case 0x070: return _status;
                case 0x0FC: return 0;     // config generation: never changes
            }

            // virtio-net config space at 0x100: mac[6] then u16 status.
            if (offset >= 0x100 && offset < 0x108)
            {
                byte[] mac = _backend.GuestMac;
                uint o = offset - 0x100;
                // Build a 4-byte window starting at the requested offset, then
                // return the low `width` bytes — matches what an 8/16/32-bit MMIO
                // read would see.
                uint w = 0;
                for (int i = 0; i < 4; i++)
                {
                    uint pos = o + (uint)i;
                    byte b = pos < 6           ? mac[pos]
                          : (pos == 6 || pos == 7) ? (byte)0          // status u16 = 0 (link not advertised)
                          : (byte)0;
                    w |= (uint)b << (i * 8);
                }
                return width == 4 ? w : width == 2 ? (w & 0xFFFFu) : (w & 0xFFu);
            }
            return 0;
        }

        // ── MMIO Write ────────────────────────────────────────────────────────

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x014: _deviceFeaturesSel = value; return;
                case 0x020:
                {
                    int shift = _driverFeaturesSel == 0 ? 0 : 32;
                    _driverFeatures = (_driverFeatures & ~(0xFFFF_FFFFUL << shift))
                                    | ((ulong)value << shift);
                    return;
                }
                case 0x024: _driverFeaturesSel = value; return;
                case 0x030: _queueSel = value; return;
                case 0x038: if (_queueSel < NumQueues) _queueNum[_queueSel] = value; return;
                case 0x044: if (_queueSel < NumQueues) _queueReady[_queueSel] = value; return;
                case 0x050: HandleQueueNotify(value); return;
                case 0x064: _interruptStatus &= ~value; UpdateIrq(); return;
                case 0x070: HandleStatusWrite(value); return;

                case 0x080: WriteQAddrLow (_descAddr,   value); return;
                case 0x084: WriteQAddrHigh(_descAddr,   value); return;
                case 0x090: WriteQAddrLow (_driverAddr, value); return;
                case 0x094: WriteQAddrHigh(_driverAddr, value); return;
                case 0x0A0: WriteQAddrLow (_deviceAddr, value); return;
                case 0x0A4: WriteQAddrHigh(_deviceAddr, value); return;
            }
        }

        private void WriteQAddrLow(ulong[] slot, uint value)
        {
            if (_queueSel < NumQueues) slot[_queueSel] = (slot[_queueSel] & 0xFFFF_FFFF_0000_0000UL) | value;
        }
        private void WriteQAddrHigh(ulong[] slot, uint value)
        {
            if (_queueSel < NumQueues) slot[_queueSel] = (slot[_queueSel] & 0x0000_0000_FFFF_FFFFUL) | ((ulong)value << 32);
        }

        private void HandleStatusWrite(uint value)
        {
            if (value == 0)
            {
                // Reset.
                _status            = 0;
                _interruptStatus   = 0;
                _driverFeatures    = 0;
                _deviceFeaturesSel = 0;
                _driverFeaturesSel = 0;
                _queueSel          = 0;
                for (int i = 0; i < NumQueues; i++)
                {
                    _queueNum[i] = _queueReady[i] = 0;
                    _descAddr[i] = _driverAddr[i] = _deviceAddr[i] = 0;
                    _lastAvailIdx[i] = _lastUsedIdx[i] = 0;
                }
                UpdateIrq();
                return;
            }

            _status = value;
            // If guest set FEATURES_OK but didn't accept VERSION_1, we must clear it.
            if ((value & STATUS_FEATURES_OK) != 0)
            {
                if ((_driverFeatures & (1UL << VIRTIO_F_VERSION_1)) == 0)
                    _status &= ~STATUS_FEATURES_OK;
            }
        }

        // ── Feature advertisement ─────────────────────────────────────────────

        private static uint DeviceFeaturesWindow(uint sel)
        {
            ulong feats = (1UL << VIRTIO_NET_F_MAC) | (1UL << VIRTIO_F_VERSION_1);
            return sel == 0 ? (uint) feats : (uint)(feats >> 32);
        }

        // ── Queue dispatch ────────────────────────────────────────────────────

        private void HandleQueueNotify(uint queueIdx)
        {
            if (queueIdx >= NumQueues) return;
            if ((_status & STATUS_DRIVER_OK) == 0) return;
            if (_queueReady[queueIdx] == 0) return;
            if (queueIdx == 1) DrainTx();
            // RX notify means the driver added empty buffers; nothing to do here.
            // We'll consume them when a host frame arrives.
        }

        // ── TX path (guest → host) ────────────────────────────────────────────

        private void DrainTx()
        {
            const int TX = 1;
            uint num = _queueNum[TX];
            if (num == 0 || _descAddr[TX] == 0 || _driverAddr[TX] == 0 || _deviceAddr[TX] == 0) return;

            byte* desc   = _ramBase + (uint)_descAddr[TX];
            byte* avail  = _ramBase + (uint)_driverAddr[TX];
            byte* used   = _ramBase + (uint)_deviceAddr[TX];

            ushort availIdx = Volatile.Read(ref *(ushort*)(avail + 2));
            while (_lastAvailIdx[TX] != availIdx)
            {
                ushort head = *(ushort*)(avail + 4 + (_lastAvailIdx[TX] % num) * 2);

                int totalLen = WalkAndCollect(desc, num, head, writeable: false, out byte[] frame);

                if (totalLen > VirtioNetHdrLen)
                {
                    _backend.SendFromGuest(new ReadOnlySpan<byte>(frame, VirtioNetHdrLen, totalLen - VirtioNetHdrLen));
                }

                // Push descriptor head to the used ring.
                ushort usedIdx = _lastUsedIdx[TX];
                byte*  usedRingEntry = used + 4 + (usedIdx % num) * 8;
                *(uint*)(usedRingEntry + 0) = head;
                *(uint*)(usedRingEntry + 4) = (uint)totalLen;
                _lastUsedIdx[TX] = (ushort)(usedIdx + 1);
                Volatile.Write(ref *(ushort*)(used + 2), _lastUsedIdx[TX]);

                _lastAvailIdx[TX]++;
            }

            _interruptStatus |= 1;
            UpdateIrq();
        }

        // ── RX path (host → guest) ────────────────────────────────────────────

        private void OnHostFrame(byte[] frame)
        {
            const int RX = 0;
            if ((_status & STATUS_DRIVER_OK) == 0) return;
            uint num = _queueNum[RX];
            if (num == 0 || _descAddr[RX] == 0 || _driverAddr[RX] == 0 || _deviceAddr[RX] == 0) return;

            byte* desc  = _ramBase + (uint)_descAddr[RX];
            byte* avail = _ramBase + (uint)_driverAddr[RX];
            byte* used  = _ramBase + (uint)_deviceAddr[RX];

            ushort availIdx = Volatile.Read(ref *(ushort*)(avail + 2));
            if (_lastAvailIdx[RX] == availIdx) return;  // no buffer available; drop the frame

            ushort head = *(ushort*)(avail + 4 + (_lastAvailIdx[RX] % num) * 2);

            // Prepare the virtio_net_hdr + frame bytes.
            int packetLen = VirtioNetHdrLen + frame.Length;
            byte[] buf = new byte[packetLen];
            // hdr is all zero except num_buffers = 1 at offset 10.
            buf[10] = 1;
            Buffer.BlockCopy(frame, 0, buf, VirtioNetHdrLen, frame.Length);

            int written = WalkAndWrite(desc, num, head, buf);

            ushort usedIdx = _lastUsedIdx[RX];
            byte*  usedRingEntry = used + 4 + (usedIdx % num) * 8;
            *(uint*)(usedRingEntry + 0) = head;
            *(uint*)(usedRingEntry + 4) = (uint)written;
            _lastUsedIdx[RX] = (ushort)(usedIdx + 1);
            Volatile.Write(ref *(ushort*)(used + 2), _lastUsedIdx[RX]);

            _lastAvailIdx[RX]++;
            _interruptStatus |= 1;
            UpdateIrq();
        }

        // ── Descriptor chain walkers ──────────────────────────────────────────

        private int WalkAndCollect(byte* descTable, uint num, ushort head, bool writeable, out byte[] data)
        {
            // First pass: count total length.
            int  total = 0;
            ushort idx = head;
            for (int hop = 0; hop < num; hop++)
            {
                byte* d = descTable + idx * 16;
                ushort flags = *(ushort*)(d + 12);
                bool isWrite = (flags & VRING_DESC_F_WRITE) != 0;
                if (isWrite == writeable) total += (int)*(uint*)(d + 8);
                if ((flags & VRING_DESC_F_NEXT) == 0) break;
                idx = *(ushort*)(d + 14);
            }
            data = new byte[total];

            // Second pass: copy bytes.
            int pos = 0;
            idx     = head;
            for (int hop = 0; hop < num; hop++)
            {
                byte* d = descTable + idx * 16;
                ulong addr  = *(ulong*) (d + 0);
                uint  len   = *(uint*)  (d + 8);
                ushort flags = *(ushort*)(d + 12);
                bool isWrite = (flags & VRING_DESC_F_WRITE) != 0;
                if (isWrite == writeable)
                {
                    byte* src = _ramBase + (uint)addr;
                    for (int i = 0; i < len && pos < total; i++) data[pos++] = src[i];
                }
                if ((flags & VRING_DESC_F_NEXT) == 0) break;
                idx = *(ushort*)(d + 14);
            }
            return total;
        }

        private int WalkAndWrite(byte* descTable, uint num, ushort head, byte[] src)
        {
            int  pos = 0;
            ushort idx = head;
            for (int hop = 0; hop < num && pos < src.Length; hop++)
            {
                byte* d = descTable + idx * 16;
                ulong addr  = *(ulong*) (d + 0);
                uint  len   = *(uint*)  (d + 8);
                ushort flags = *(ushort*)(d + 12);
                if ((flags & VRING_DESC_F_WRITE) != 0)
                {
                    byte* dst = _ramBase + (uint)addr;
                    int n     = (int)len;
                    if (pos + n > src.Length) n = src.Length - pos;
                    for (int i = 0; i < n; i++) dst[i] = src[pos + i];
                    pos += n;
                }
                if ((flags & VRING_DESC_F_NEXT) == 0) break;
                idx = *(ushort*)(d + 14);
            }
            return pos;
        }

        // ── Interrupt routing ─────────────────────────────────────────────────

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private void UpdateIrq()
        {
            if (_interruptStatus != 0) _plic.RaiseIrq(_irqNum);
            else                       _plic.LowerIrq(_irqNum);
        }

        private static uint ExtractByte(uint word, int byteOffset, int width)
            => width switch
            {
                1 => (word >> (byteOffset * 8)) & 0xFF,
                2 => (word >> (byteOffset * 8)) & 0xFFFF,
                _ => word,
            };

        public void Dispose() => _backend.Dispose();
    }
}
