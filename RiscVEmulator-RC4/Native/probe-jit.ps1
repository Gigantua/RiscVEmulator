$src = @'
using System;
using System.Runtime.InteropServices;
class P {
    [DllImport("rv32i_core")] static extern void rv32i_init(IntPtr mem, uint entry);
    [DllImport("rv32i_core")] static extern void rv32i_destroy();
    [DllImport("rv32i_core")] static extern int  rv32i_jit_active();
    static unsafe void Main() {
        var mem = Marshal.AllocHGlobal(0x10000);
        rv32i_init(mem, 0x1000);
        Console.WriteLine("jit_active = " + rv32i_jit_active());
        rv32i_destroy();
    }
}
'@
$src | Out-File -Encoding utf8 probe.cs
Add-Type -TypeDefinition $src -Language CSharp 2>&1 | Out-Null
[P]::Main()
