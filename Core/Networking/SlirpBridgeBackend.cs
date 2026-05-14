using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;

namespace RiscVEmulator.Core.Networking
{
    /// <summary>
    /// Backend that delegates to <c>slirp_bridge.dll</c> — a tiny C++ shim that
    /// owns a libslirp instance and runs its poll loop in native code. We talk
    /// to it via a 5-function C ABI.
    ///
    /// Key invariants:
    ///   * sb_tx is called only from the dedicated TX thread — never from
    ///     within the VEH/MMIO dispatcher path, which is nested inside another
    ///     P/Invoke (riscv_step). Doing P/Invoke→VEH→P/Invoke trips the
    ///     .NET 10 JIT's UnmanagedCallersOnly check.
    ///   * sb_rx is called only from the dedicated RX thread.
    ///   * All P/Invokes use raw <c>byte*</c> + <c>int</c> — no <c>byte[]</c>
    ///     marshalling stubs (which the JIT also dislikes in nested contexts).
    ///
    /// Topology (set in the C side): guest=10.0.2.15, host=10.0.2.2, dns=10.0.2.3.
    /// </summary>
    public sealed unsafe class SlirpBridgeBackend : INetBackend
    {
        private const string Lib = "slirp_bridge";

        [DllImport(Lib)] private static extern int  sb_init();
        [DllImport(Lib)] private static extern void sb_tx(byte* buf, int len);
        [DllImport(Lib)] private static extern int  sb_rx(byte* buf, int max_len);
        [DllImport(Lib)] private static extern void sb_get_mac(byte* mac);
        [DllImport(Lib)] private static extern void sb_cleanup();

        public static bool IsAvailable()
        {
            string dllPath = System.IO.Path.Combine(System.AppContext.BaseDirectory, Lib + ".dll");
            if (!System.IO.File.Exists(dllPath)) return false;
            try { NativeLibrary.Load(dllPath); return true; }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[SlirpBridge] Load(\"{dllPath}\") failed: {ex.Message}");
                return false;
            }
        }

        public byte[] GuestMac { get; }
        public Action<byte[]>? ReceivedFromHost { get; set; }

        private readonly BlockingCollection<byte[]> _txQueue = new(new ConcurrentQueue<byte[]>());
        private readonly Thread _txThread;
        private readonly Thread _rxThread;
        private readonly CancellationTokenSource _cts = new();

        public SlirpBridgeBackend()
        {
            int rc = sb_init();
            if (rc != 0) throw new InvalidOperationException($"sb_init failed ({rc})");

            GuestMac = new byte[6];
            fixed (byte* p = GuestMac) sb_get_mac(p);
            Console.Error.WriteLine($"[SlirpBridge] MAC = {BitConverter.ToString(GuestMac)}");

            _txThread = new Thread(TxPump) { IsBackground = true, Name = "SlirpBridge-TX" };
            _rxThread = new Thread(RxPump) { IsBackground = true, Name = "SlirpBridge-RX" };
            // TX is synchronous from SendFromGuest — fine now that the UCO
            // issue in MmioDispatcher is resolved. Keep the TX thread defined
            // so Dispose can join cleanly if SendFromGuest ever switches back
            // to queueing.
            _rxThread.Start();
        }

        public void SendFromGuest(ReadOnlySpan<byte> frame)
        {
            fixed (byte* p = frame)
                sb_tx(p, frame.Length);
        }

        private void TxPump()
        {
            try
            {
                foreach (var frame in _txQueue.GetConsumingEnumerable(_cts.Token))
                {
                    fixed (byte* p = frame)
                        sb_tx(p, frame.Length);
                }
            }
            catch (OperationCanceledException) { }
        }

        private void RxPump()
        {
            byte[] buf = new byte[2048];
            while (!_cts.IsCancellationRequested)
            {
                int n;
                fixed (byte* p = buf) n = sb_rx(p, buf.Length);
                if (n > 0)
                {
                    var f = new byte[n];
                    Buffer.BlockCopy(buf, 0, f, 0, n);
                    ReceivedFromHost?.Invoke(f);
                }
                else
                {
                    Thread.Sleep(1);
                }
            }
        }

        public void Dispose()
        {
            _cts.Cancel();
            _txQueue.CompleteAdding();
            try { _txThread.Join(500); } catch { }
            try { _rxThread.Join(500); } catch { }
            try { sb_cleanup(); } catch { }
        }
    }
}
