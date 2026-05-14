using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;

namespace RiscVEmulator.Core.Networking
{
    /// <summary>
    /// Windows-native userspace NAT backend for <c>VirtioNetDevice</c>. Handles
    /// ARP, ICMP echo, and UDP forwarding. No admin rights required (ICMP goes
    /// through <c>IcmpSendEcho</c>; UDP via standard sockets). TCP is not yet
    /// implemented — use libslirp for HTTP/etc.
    ///
    /// Virtual topology:
    ///   guest = 10.0.2.15   (set via kernel bootargs: ip=10.0.2.15::10.0.2.2:...)
    ///   gateway/host = 10.0.2.2
    ///   DNS = 10.0.2.3 (we proxy to the system resolver)
    /// </summary>
    public sealed class Win32NatBackend : INetBackend
    {
        // ── Virtual topology ──────────────────────────────────────────────────

        public byte[] GuestMac { get; } = { 0x02, 0x54, 0x00, 0x12, 0x34, 0x56 };
        public byte[] HostMac  { get; } = { 0x02, 0x54, 0x00, 0x00, 0x00, 0x01 };
        public uint   GuestIp  { get; } = MakeIp(10, 0, 2, 15);
        public uint   HostIp   { get; } = MakeIp(10, 0, 2, 2);
        public uint   DnsIp    { get; } = MakeIp(10, 0, 2, 3);

        public Action<byte[]>? ReceivedFromHost { get; set; }

        // ── Pending work ──────────────────────────────────────────────────────

        private readonly CancellationTokenSource _cts = new();
        private readonly Thread _worker;
        private readonly BlockingCollection<Action> _work = new(new ConcurrentQueue<Action>());

        // UDP NAT: src_port → host UdpClient bound for replies.
        private readonly Dictionary<ushort, UdpForward> _udp = new();
        private readonly object _udpLock = new();

        public Win32NatBackend()
        {
            _worker = new Thread(WorkerLoop) { IsBackground = true, Name = "Win32NatBackend" };
            _worker.Start();
        }

        public void Dispose()
        {
            _cts.Cancel();
            _work.CompleteAdding();
            lock (_udpLock)
            {
                foreach (var f in _udp.Values) f.Client.Dispose();
                _udp.Clear();
            }
        }

        // ── Entry from the device ─────────────────────────────────────────────

        public void SendFromGuest(ReadOnlySpan<byte> frame)
        {
            if (frame.Length < 14) return;
            ushort etype = (ushort)((frame[12] << 8) | frame[13]);
            byte[] copy  = frame.ToArray();
            _work.Add(() =>
            {
                try
                {
                    if (etype == 0x0806) HandleArp(copy);
                    else if (etype == 0x0800) HandleIpv4(copy);
                }
                catch { /* swallow per-packet errors */ }
            });
        }

        // ── Worker thread ─────────────────────────────────────────────────────

        private void WorkerLoop()
        {
            foreach (var job in _work.GetConsumingEnumerable(_cts.Token))
            {
                if (_cts.IsCancellationRequested) break;
                job();
            }
        }

        // ── ARP ───────────────────────────────────────────────────────────────

        private void HandleArp(byte[] frame)
        {
            if (frame.Length < 14 + 28) return;
            int o = 14;
            ushort op = (ushort)((frame[o + 6] << 8) | frame[o + 7]);
            if (op != 1) return;  // only handle requests

            uint tpa = BeRead32(frame, o + 24);
            if (tpa != HostIp && tpa != DnsIp) return;  // only respond for our virtual hosts

            byte[] reply = new byte[14 + 28];
            // Ethernet
            Array.Copy(frame, 6, reply, 0, 6);   // dst = sender hw addr
            Array.Copy(HostMac, 0, reply, 6, 6); // src = our gateway MAC
            reply[12] = 0x08; reply[13] = 0x06;
            // ARP body
            reply[14] = 0; reply[15] = 1;        // htype = Ethernet
            reply[16] = 0x08; reply[17] = 0x00;  // ptype = IPv4
            reply[18] = 6; reply[19] = 4;        // hlen, plen
            reply[20] = 0; reply[21] = 2;        // op = reply
            Array.Copy(HostMac, 0, reply, 22, 6);
            BeWrite32(reply, 28, tpa);
            Array.Copy(frame, o + 8, reply, 32, 6);   // target hw = original sender hw
            Array.Copy(frame, o + 14, reply, 38, 4);  // target ip = original sender ip

            ReceivedFromHost?.Invoke(reply);
        }

        // ── IPv4 dispatch ─────────────────────────────────────────────────────

        private void HandleIpv4(byte[] frame)
        {
            int o = 14;
            if (frame.Length < o + 20) return;
            int ihl  = (frame[o] & 0x0F) * 4;
            byte proto = frame[o + 9];
            uint src   = BeRead32(frame, o + 12);
            uint dst   = BeRead32(frame, o + 16);
            int  iphdr = o + ihl;
            if (frame.Length < iphdr) return;

            switch (proto)
            {
                case 1:  HandleIcmp(frame, src, dst, iphdr); return;
                case 17: HandleUdp (frame, src, dst, iphdr); return;
                // TCP (6): not implemented in this backend.
            }
        }

        // ── ICMP echo via IcmpSendEcho ────────────────────────────────────────

        private unsafe void HandleIcmp(byte[] frame, uint src, uint dst, int icmpOff)
        {
            if (frame.Length < icmpOff + 8) return;
            byte type = frame[icmpOff];
            if (type != 8) return;  // only echo request

            int  payloadLen = frame.Length - icmpOff - 8;
            byte[] payload  = new byte[payloadLen];
            Buffer.BlockCopy(frame, icmpOff + 8, payload, 0, payloadLen);

            ushort icmpId  = (ushort)((frame[icmpOff + 4] << 8) | frame[icmpOff + 5]);
            ushort icmpSeq = (ushort)((frame[icmpOff + 6] << 8) | frame[icmpOff + 7]);

            IntPtr hIcmp = IcmpCreateFile();
            if (hIcmp == IntPtr.Zero || hIcmp == new IntPtr(-1)) return;

            try
            {
                int  replyBufSize = Marshal.SizeOf<ICMP_ECHO_REPLY>() + payloadLen + 8;
                byte[] replyBuf   = new byte[replyBufSize];

                fixed (byte* pPayload = payload)
                fixed (byte* pReply   = replyBuf)
                {
                    uint n = IcmpSendEcho(hIcmp, SwapEndian(dst), (IntPtr)pPayload, (ushort)payloadLen,
                                          IntPtr.Zero, (IntPtr)pReply, (uint)replyBufSize, 2000);
                    if (n == 0) return;

                    var er = Marshal.PtrToStructure<ICMP_ECHO_REPLY>((IntPtr)pReply);
                    byte[] replyPayload = new byte[er.DataSize];
                    Marshal.Copy(er.Data, replyPayload, 0, er.DataSize);

                    // Build response Ethernet frame: ICMP echo reply (type 0) from dst back to src.
                    byte[] icmp = new byte[8 + replyPayload.Length];
                    icmp[0] = 0; icmp[1] = 0;                       // type, code
                    icmp[4] = (byte)(icmpId >> 8); icmp[5] = (byte)icmpId;
                    icmp[6] = (byte)(icmpSeq >> 8); icmp[7] = (byte)icmpSeq;
                    Buffer.BlockCopy(replyPayload, 0, icmp, 8, replyPayload.Length);
                    ushort csum = Checksum(icmp, 0, icmp.Length);
                    icmp[2] = (byte)(csum >> 8); icmp[3] = (byte)csum;

                    byte[] outFrame = BuildEthIpv4(srcIp: dst, dstIp: src, proto: 1, payload: icmp);
                    ReceivedFromHost?.Invoke(outFrame);
                }
            }
            finally { IcmpCloseHandle(hIcmp); }
        }

        // ── UDP forwarding ────────────────────────────────────────────────────

        private void HandleUdp(byte[] frame, uint src, uint dst, int udpOff)
        {
            if (frame.Length < udpOff + 8) return;
            ushort srcPort = (ushort)((frame[udpOff + 0] << 8) | frame[udpOff + 1]);
            ushort dstPort = (ushort)((frame[udpOff + 2] << 8) | frame[udpOff + 3]);
            int    udpLen  = (frame[udpOff + 4] << 8) | frame[udpOff + 5];
            int    payLen  = udpLen - 8;
            if (frame.Length < udpOff + 8 + payLen || payLen < 0) return;

            byte[] payload = new byte[payLen];
            Buffer.BlockCopy(frame, udpOff + 8, payload, 0, payLen);

            // Forward target. If guest hits our DNS IP (10.0.2.3:53), redirect to
            // the system's configured resolver; otherwise route by IP directly.
            uint   tgtIp   = dst;
            ushort tgtPort = dstPort;
            if (dst == DnsIp && dstPort == 53)
            {
                tgtIp = GetSystemDnsServer();
                if (tgtIp == 0) return;
            }

            UdpForward fwd = GetOrCreateUdp(srcPort, src);
            try
            {
                fwd.Client.Send(payload, payload.Length,
                                new IPEndPoint(new IPAddress(BitConverter.GetBytes(SwapEndian(tgtIp))), tgtPort));
                fwd.LastReplySourceIp   = dst;
                fwd.LastReplySourcePort = dstPort;
            }
            catch { /* drop */ }
        }

        private UdpForward GetOrCreateUdp(ushort guestSrcPort, uint guestSrcIp)
        {
            lock (_udpLock)
            {
                if (_udp.TryGetValue(guestSrcPort, out var existing)) return existing;
                var client = new UdpClient(new IPEndPoint(IPAddress.Any, 0));
                var fwd = new UdpForward { Client = client, GuestSrcIp = guestSrcIp, GuestSrcPort = guestSrcPort };
                _udp[guestSrcPort] = fwd;
                _ = BeginReceive(fwd);
                return fwd;
            }
        }

        private async System.Threading.Tasks.Task BeginReceive(UdpForward fwd)
        {
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    var rcv = await fwd.Client.ReceiveAsync(_cts.Token).ConfigureAwait(false);
                    byte[] data = rcv.Buffer;
                    // Build Ethernet/IPv4/UDP reply back to the guest from the apparent source.
                    byte[] udp = new byte[8 + data.Length];
                    udp[0] = (byte)(fwd.LastReplySourcePort >> 8);
                    udp[1] = (byte)fwd.LastReplySourcePort;
                    udp[2] = (byte)(fwd.GuestSrcPort >> 8);
                    udp[3] = (byte)fwd.GuestSrcPort;
                    int len = 8 + data.Length;
                    udp[4] = (byte)(len >> 8); udp[5] = (byte)len;
                    // UDP checksum = 0 (allowed for IPv4)
                    Buffer.BlockCopy(data, 0, udp, 8, data.Length);

                    byte[] outFrame = BuildEthIpv4(srcIp: fwd.LastReplySourceIp, dstIp: fwd.GuestSrcIp,
                                                   proto: 17, payload: udp);
                    ReceivedFromHost?.Invoke(outFrame);
                }
            }
            catch { /* socket closed or cancelled */ }
        }

        private sealed class UdpForward
        {
            public UdpClient Client = null!;
            public uint   GuestSrcIp;
            public ushort GuestSrcPort;
            public uint   LastReplySourceIp;
            public ushort LastReplySourcePort;
        }

        // ── Frame builders ────────────────────────────────────────────────────

        private byte[] BuildEthIpv4(uint srcIp, uint dstIp, byte proto, byte[] payload)
        {
            byte[] frame = new byte[14 + 20 + payload.Length];
            // Ethernet header
            Array.Copy(GuestMac, 0, frame, 0, 6);
            Array.Copy(HostMac,  0, frame, 6, 6);
            frame[12] = 0x08; frame[13] = 0x00;  // IPv4
            int ip = 14;
            frame[ip + 0] = 0x45;                // version 4, IHL 5
            frame[ip + 1] = 0;                   // DSCP/ECN
            int total = 20 + payload.Length;
            frame[ip + 2] = (byte)(total >> 8); frame[ip + 3] = (byte)total;
            frame[ip + 4] = 0; frame[ip + 5] = 0;  // identifier
            frame[ip + 6] = 0x40;                // DF set, flags=0, frag=0
            frame[ip + 7] = 0;
            frame[ip + 8] = 64;                  // TTL
            frame[ip + 9] = proto;
            BeWrite32(frame, ip + 12, srcIp);
            BeWrite32(frame, ip + 16, dstIp);
            ushort csum = Checksum(frame, ip, 20);
            frame[ip + 10] = (byte)(csum >> 8); frame[ip + 11] = (byte)csum;
            Buffer.BlockCopy(payload, 0, frame, ip + 20, payload.Length);
            return frame;
        }

        // ── Helpers ───────────────────────────────────────────────────────────

        private static uint BeRead32(byte[] b, int o)
            => ((uint)b[o] << 24) | ((uint)b[o + 1] << 16) | ((uint)b[o + 2] << 8) | b[o + 3];

        private static void BeWrite32(byte[] b, int o, uint v)
        {
            b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16);
            b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v;
        }

        private static uint MakeIp(byte a, byte b, byte c, byte d) =>
            ((uint)a << 24) | ((uint)b << 16) | ((uint)c << 8) | d;

        private static uint SwapEndian(uint v)
            => ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8) |
               ((v & 0x00FF0000) >> 8)  | ((v & 0xFF000000) >> 24);

        private static ushort Checksum(byte[] data, int offset, int len)
        {
            uint sum = 0;
            for (int i = 0; i < len; i += 2)
            {
                ushort word = (i + 1 < len) ? (ushort)((data[offset + i] << 8) | data[offset + i + 1])
                                            : (ushort)(data[offset + i] << 8);
                sum += word;
                if ((sum & 0xFFFF0000) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
            }
            return (ushort)~sum;
        }

        private static uint GetSystemDnsServer()
        {
            try
            {
                foreach (var nic in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (nic.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up) continue;
                    foreach (IPAddress addr in nic.GetIPProperties().DnsAddresses)
                    {
                        if (addr.AddressFamily != AddressFamily.InterNetwork) continue;
                        byte[] b = addr.GetAddressBytes();
                        return MakeIp(b[0], b[1], b[2], b[3]);
                    }
                }
            }
            catch { }
            return MakeIp(8, 8, 8, 8);  // fallback
        }

        // ── P/Invoke (ICMP Helper API) ────────────────────────────────────────

        [DllImport("iphlpapi.dll")] private static extern IntPtr IcmpCreateFile();
        [DllImport("iphlpapi.dll")] private static extern bool   IcmpCloseHandle(IntPtr h);
        [DllImport("iphlpapi.dll", SetLastError = true)]
        private static extern uint IcmpSendEcho(IntPtr handle, uint destIp,
            IntPtr requestData, ushort requestSize, IntPtr requestOptions,
            IntPtr replyBuffer, uint replySize, uint timeoutMs);

        [StructLayout(LayoutKind.Sequential)]
        private struct ICMP_ECHO_REPLY
        {
            public uint   Address;
            public uint   Status;
            public uint   RoundTripTime;
            public ushort DataSize;
            public ushort Reserved;
            public IntPtr Data;
            // IP_OPTION_INFORMATION follows but we don't need it
            public byte   Ttl;
            public byte   Tos;
            public byte   Flags;
            public byte   OptionsSize;
            public IntPtr OptionsData;
        }
    }
}
