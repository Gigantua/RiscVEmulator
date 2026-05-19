using System;
using System.IO;
using System.Threading;

namespace RiscVEmulator.Core.Peripherals
{
    /// <summary>
    /// virtio-mmio v2 block device, backed by a host disk-image file. One split
    /// virtqueue carries virtio_blk requests — a 16-byte header (type + sector),
    /// one or more data buffers, and a 1-byte status. Gives the RV64 Alpine
    /// guest a persistent <c>/dev/vda</c>.
    ///
    /// Spec: virtio-v1.1 §4.2 (MMIO transport) and §5.2 (block device).
    /// </summary>
    public sealed unsafe class VirtioBlkDevice : IPeripheral, IDisposable
    {
        private const uint MAGIC     = 0x74726976u;   // 'virt'
        private const uint VERSION   = 2u;
        private const uint DEVICE_ID = 2u;            // block
        private const uint VENDOR_ID = 0x554D4551u;   // 'QEMU'

        private const uint STATUS_DRIVER_OK   = 4;
        private const uint STATUS_FEATURES_OK = 8;
        private const int  VIRTIO_F_VERSION_1 = 32;

        private const ushort VRING_DESC_F_NEXT  = 1;
        private const ushort VRING_DESC_F_WRITE = 2;

        private const uint VIRTIO_BLK_T_IN  = 0;      // read  (disk → guest)
        private const uint VIRTIO_BLK_T_OUT = 1;      // write (guest → disk)

        private const int MaxQueueSize = 256;
        private const int SectorSize   = 512;

        public uint BaseAddress { get; }
        public uint Size      => 0x1000;
        public bool IsGuarded => true;

        private readonly PlicDevice _plic;
        private readonly int        _irqNum;
        private readonly byte*      _ramBase;
        private readonly FileStream _disk;
        private readonly ulong      _capacitySectors;
        private readonly object     _lock = new();

        private uint  _deviceFeaturesSel, _driverFeaturesSel;
        private ulong _driverFeatures;
        private uint  _status, _interruptStatus, _queueSel;
        private uint  _queueNum, _queueReady;
        private ulong _descAddr, _driverAddr, _deviceAddr;
        private ushort _lastAvailIdx, _lastUsedIdx;

        public VirtioBlkDevice(uint baseAddress, PlicDevice plic, int irqNum,
                               string diskPath, IntPtr ramBase)
        {
            BaseAddress = baseAddress;
            _plic       = plic;
            _irqNum     = irqNum;
            _ramBase    = (byte*)ramBase;
            _disk = new FileStream(diskPath, FileMode.OpenOrCreate,
                                   FileAccess.ReadWrite, FileShare.Read);
            _capacitySectors = (ulong)(_disk.Length / SectorSize);
        }

        // ── MMIO ──────────────────────────────────────────────────────────────

        public uint Read(uint offset, int width)
        {
            switch (offset)
            {
                case 0x000: return MAGIC;
                case 0x004: return VERSION;
                case 0x008: return DEVICE_ID;
                case 0x00C: return VENDOR_ID;
                case 0x010: return _deviceFeaturesSel == 1 ? 1u : 0u;  // only VIRTIO_F_VERSION_1
                case 0x034: return MaxQueueSize;
                case 0x044: return _queueSel == 0 ? _queueReady : 0;
                case 0x060: return _interruptStatus;
                case 0x070: return _status;
                case 0x0FC: return 0;                                  // config generation
                case 0x100: return (uint) _capacitySectors;            // capacity low
                case 0x104: return (uint)(_capacitySectors >> 32);     // capacity high
            }
            return 0;
        }

        public void Write(uint offset, int width, uint value)
        {
            switch (offset)
            {
                case 0x014: _deviceFeaturesSel = value; return;
                case 0x020:
                    _driverFeatures = _driverFeaturesSel == 0
                        ? (_driverFeatures & 0xFFFFFFFF00000000UL) | value
                        : (_driverFeatures & 0x00000000FFFFFFFFUL) | ((ulong)value << 32);
                    return;
                case 0x024: _driverFeaturesSel = value; return;
                case 0x030: _queueSel = value; return;
                case 0x038: if (_queueSel == 0) _queueNum   = value; return;
                case 0x044: if (_queueSel == 0) _queueReady = value; return;
                case 0x050: if (value == 0) ProcessQueue(); return;
                case 0x064: _interruptStatus &= ~value; UpdateIrq(); return;
                case 0x070: HandleStatus(value); return;
                case 0x080: _descAddr   = (_descAddr   & 0xFFFFFFFF00000000UL) | value;             return;
                case 0x084: _descAddr   = (_descAddr   & 0x00000000FFFFFFFFUL) | ((ulong)value<<32); return;
                case 0x090: _driverAddr = (_driverAddr & 0xFFFFFFFF00000000UL) | value;             return;
                case 0x094: _driverAddr = (_driverAddr & 0x00000000FFFFFFFFUL) | ((ulong)value<<32); return;
                case 0x0A0: _deviceAddr = (_deviceAddr & 0xFFFFFFFF00000000UL) | value;             return;
                case 0x0A4: _deviceAddr = (_deviceAddr & 0x00000000FFFFFFFFUL) | ((ulong)value<<32); return;
            }
        }

        private void HandleStatus(uint value)
        {
            if (value == 0)                                     // reset
            {
                _status = _interruptStatus = 0;
                _driverFeatures = 0;
                _deviceFeaturesSel = _driverFeaturesSel = _queueSel = 0;
                _queueNum = _queueReady = 0;
                _descAddr = _driverAddr = _deviceAddr = 0;
                _lastAvailIdx = _lastUsedIdx = 0;
                UpdateIrq();
                return;
            }
            _status = value;
            // The driver must accept VIRTIO_F_VERSION_1 for a modern transport.
            if ((value & STATUS_FEATURES_OK) != 0 &&
                (_driverFeatures & (1UL << VIRTIO_F_VERSION_1)) == 0)
                _status &= ~STATUS_FEATURES_OK;
        }

        // ── Request processing ────────────────────────────────────────────────

        private void ProcessQueue()
        {
            if ((_status & STATUS_DRIVER_OK) == 0 || _queueReady == 0) return;
            if (_queueNum == 0 || _descAddr == 0 || _driverAddr == 0 || _deviceAddr == 0) return;

            lock (_lock)
            {
                byte* desc  = _ramBase + (uint)_descAddr;
                byte* avail = _ramBase + (uint)_driverAddr;
                byte* used  = _ramBase + (uint)_deviceAddr;
                uint  num   = _queueNum;

                ushort availIdx = Volatile.Read(ref *(ushort*)(avail + 2));
                bool any = false;
                while (_lastAvailIdx != availIdx)
                {
                    ushort head  = *(ushort*)(avail + 4 + (_lastAvailIdx % num) * 2);
                    uint   wrote = HandleRequest(desc, num, head);

                    ushort ui = _lastUsedIdx;
                    byte*  ue = used + 4 + (ui % num) * 8;
                    *(uint*)(ue + 0) = head;
                    *(uint*)(ue + 4) = wrote;
                    _lastUsedIdx = (ushort)(ui + 1);
                    Volatile.Write(ref *(ushort*)(used + 2), _lastUsedIdx);

                    _lastAvailIdx++;
                    any = true;
                }
                if (any) { _interruptStatus |= 1; UpdateIrq(); }
            }
        }

        private uint HandleRequest(byte* descTable, uint num, ushort head)
        {
            // Collect the descriptor chain. A single block request can chain
            // one descriptor per 4 KiB data page, so the cap must cover a full
            // queue (256) — the original 32 truncated large writes, which
            // surfaced as an I/O error extracting big files (libLLVM.so).
            Span<ulong> addr = stackalloc ulong[320];
            Span<uint>  len  = stackalloc uint[320];
            int n = 0;
            ushort idx = head;
            for (int hop = 0; hop < num && n < 320; hop++)
            {
                byte* d = descTable + idx * 16;
                addr[n] = *(ulong*)(d + 0);
                len[n]  = *(uint*) (d + 8);
                ushort flags = *(ushort*)(d + 12);
                n++;
                if ((flags & VRING_DESC_F_NEXT) == 0) break;
                idx = *(ushort*)(d + 14);
            }
            if (n < 2) return 0;

            // desc[0] = 16-byte header; desc[n-1] = 1-byte status; middle = data.
            byte* hdr    = _ramBase + (uint)addr[0];
            uint  type   = *(uint*) (hdr + 0);
            ulong sector = *(ulong*)(hdr + 8);
            byte* status = _ramBase + (uint)addr[n - 1];

            long pos = (long)sector * SectorSize;
            uint dataWritten = 0;
            for (int i = 1; i < n - 1; i++)
            {
                byte* buf = _ramBase + (uint)addr[i];
                int   l   = (int)len[i];
                var   tmp = new byte[l];
                if (type == VIRTIO_BLK_T_IN)            // disk → guest
                {
                    _disk.Seek(pos, SeekOrigin.Begin);
                    int got = 0;
                    while (got < l) { int r = _disk.Read(tmp, got, l - got); if (r <= 0) break; got += r; }
                    for (int k = 0; k < l; k++) buf[k] = k < got ? tmp[k] : (byte)0;
                    dataWritten += (uint)l;
                }
                else if (type == VIRTIO_BLK_T_OUT)      // guest → disk
                {
                    for (int k = 0; k < l; k++) tmp[k] = buf[k];
                    _disk.Seek(pos, SeekOrigin.Begin);
                    _disk.Write(tmp, 0, l);
                }
                pos += l;
            }
            if (type == VIRTIO_BLK_T_OUT) _disk.Flush();

            *status = 0;                                // VIRTIO_BLK_S_OK
            return dataWritten + 1;                     // data bytes + status byte
        }

        private void UpdateIrq()
        {
            if (_interruptStatus != 0) _plic.RaiseIrq(_irqNum);
            else                       _plic.LowerIrq(_irqNum);
        }

        public void Dispose() { _disk.Flush(); _disk.Dispose(); }
    }
}
