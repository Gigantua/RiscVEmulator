using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

// Parse command line
if (args.Length < 1)
{
    Console.Error.WriteLine("Usage: Examples.Runner <elf-file> [options]");
    Console.Error.WriteLine("  --scale N              Window scale factor (default: 3)");
    Console.Error.WriteLine("  --ram N                RAM size in MB (default: 16)");
    Console.Error.WriteLine("  --fps N                Target render FPS (default: 120)");
    Console.Error.WriteLine("  --load <file> <addr>   Load binary file into guest RAM at hex address");
    Console.Error.WriteLine("                         (repeatable, addr is hex e.g. 0x00A00000)");
    Console.Error.WriteLine("                         Size is written as u32 at addr-4 automatically");
    Console.Error.WriteLine("  --no-grab              Don't grab the mouse on startup");
    Console.Error.WriteLine("  --m-ext                Enable M-extension (hardware MUL/DIV/REM)");
    return 1;
}

string elfPath = args[0];
var opts = new SdlWindowOptions();
int ramMB = 16;
var loads = new List<(string path, uint addr)>();
bool enableMExt = false;

for (int i = 1; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--scale":   opts.Scale = int.Parse(args[++i]); break;
        case "--ram":     ramMB = int.Parse(args[++i]); break;
        case "--fps":     opts.TargetFps = int.Parse(args[++i]); break;
        case "--no-grab": opts.GrabMouse = false; break;
        case "--m-ext":   enableMExt = true; break;
        case "--load":
            string loadPath = args[++i];
            uint loadAddr = Convert.ToUInt32(args[++i], 16);
            loads.Add((loadPath, loadAddr));
            break;
    }
}

// Build SoC
byte[] elfData = File.ReadAllBytes(elfPath);
var memory  = new Memory(ramMB * 1024 * 1024);
var bus     = new MemoryBus(memory);
var uart    = new UartDevice();
var fb      = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
display.SetMemory(memory);
var kbd     = new KeyboardDevice();
var mouse   = new MouseDevice();
var rtc     = new RealTimeClockDevice();
var audioBuf  = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(fb);
bus.RegisterPeripheral(display);
bus.RegisterPeripheral(kbd);
bus.RegisterPeripheral(mouse);
bus.RegisterPeripheral(rtc);
bus.RegisterPeripheral(audioBuf);
bus.RegisterPeripheral(audioCtrl);

uart.OutputHandler = c => Console.Write(c);

var regs   = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, 0x009FFF00u); // sp

var emu = new Emulator(bus, regs, entry);
emu.EnableMExtension = enableMExt;
emu.OutputHandler = c => Console.Write(c);

// Load binary files into guest RAM (--load <file> <addr>)
foreach (var (loadPath, loadAddr) in loads)
{
    byte[] data = File.ReadAllBytes(loadPath);
    uint sizeAddr = loadAddr - 4;
    bus.WriteByte(sizeAddr + 0, (byte)(data.Length));
    bus.WriteByte(sizeAddr + 1, (byte)(data.Length >> 8));
    bus.WriteByte(sizeAddr + 2, (byte)(data.Length >> 16));
    bus.WriteByte(sizeAddr + 3, (byte)(data.Length >> 24));
    bus.Load(loadAddr, data, 0, data.Length);
    Console.WriteLine($"Loaded {loadPath} ({data.Length:N0} bytes) at 0x{loadAddr:X8} (size at 0x{sizeAddr:X8})");
}

// Run with SDL window
var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts);
return window.Run();
