using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;

if (args.Length < 1)
{
    Console.Error.WriteLine("Usage: RiscVEmulator <elf-file>");
    return 1;
}

byte[] elfData = File.ReadAllBytes(args[0]);
var memory = new Memory(16 * 1024 * 1024); // 16 MB
var bus    = new MemoryBus(memory);
var uart   = new UartDevice();
uart.OutputHandler = c => Console.Write(c);
bus.RegisterPeripheral(uart);

var regs   = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);

regs.Write(2, 0x00800000u); // sp = 8 MB mark (stack grows down)

var emu = new Emulator(bus, regs, entry);
emu.OutputHandler = c => Console.Write(c);
emu.Run();
return emu.ExitCode;
