using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core.Cuda;

// ── CUDA peripheral self-test ─────────────────────────────────────────
// Compiles a freestanding RV32I guest that touches keyboard, mouse, RTC,
// framebuffer, audio (PCM + control) and MIDI via MMIO, then runs it on the
// GPU core and asserts the device-side MMIO + host reconcile worked. No SDL,
// no audible playback — input is pre-fed and output is read back from the
// managed buffers and the reconciled C# peripheral objects.

const uint Sp = 0x00EFFF00;
const int  RamMB = 16;
string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string exeDir = AppContext.BaseDirectory;

string? root = exeDir;
while (root != null && !File.Exists(Path.Combine(root, "RiscVEmulator.sln")))
    root = Path.GetDirectoryName(root);
if (root == null) { Console.Error.WriteLine("solution root not found"); return 2; }

string src = Path.Combine(root, "Examples", "CudaPeriphTest", "Programs", "periph_test.c");
string buildDir = Path.Combine(exeDir, "build"); Directory.CreateDirectory(buildDir);
string elf = Path.Combine(buildDir, "periph_test.elf");

Console.WriteLine("Compiling periph_test.c for RV32I...");
if (!Clang(new[] {
        "--target=riscv32-unknown-elf", "-march=rv32i", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O2", "-fno-builtin", "-ffreestanding",
        "-fuse-ld=lld", "-Wl,-e,_start", "-Wl,--image-base=0x1000",
        src, "-o", elf }))
    return 2;

Console.WriteLine("Running on the GPU core...\n");
using var emu = new CudaEmulator(RamMB * 1024 * 1024);

var outBuf = new System.Text.StringBuilder();
emu.OutputHandler = c => { Console.Write(c); outBuf.Append(c); };
var midi = new List<(uint off, uint val)>();
emu.OnMidi = (off, val) => midi.Add((off, val));

// Pre-feed input BEFORE the run; StepN stages it into the managed page.
emu.Keyboard.EnqueueKey(0x41, true);          // 'A' pressed -> 0x141
emu.Mouse.MoveMouse(5, -3);
emu.Mouse.SetButton(0, true);                 // left button

uint entry = emu.LoadElf(File.ReadAllBytes(elf));
emu.CommitImage();
emu.SetReg(2, Sp);
emu.SetEntry(entry);

for (int i = 0; i < 200 && !emu.IsHalted; i++) emu.StepN(1_000_000);
if (!emu.IsHalted) { Console.Error.WriteLine("\nFAIL: did not halt"); return 2; }

// ── Assertions ────────────────────────────────────────────────────────
Console.WriteLine("\n── checks ──");
int fails = 0;
void Check(string name, bool ok) { Console.WriteLine($"{(ok ? "PASS" : "FAIL")}: {name}"); if (!ok) fails++; }

string o = outBuf.ToString();
Check("uart console output",       o.Contains("DONE"));
Check("keyboard read (0x141)",     o.Contains("KBD=0x00000141"));
Check("keyboard modifiers (0)",    o.Contains("MOD=0x00000000"));
Check("mouse dx=5",                o.Contains("dx=0x00000005"));
Check("mouse dy=-3",               o.Contains("dy=0xfffffffd"));
Check("mouse button left",         o.Contains("b=0x00000001"));
Check("rtc advanced",              !o.Contains("RTC_ms=0x00000000"));

// Framebuffer pattern (device buffer, read back D2H now that the kernel is idle).
var fb = new byte[16]; emu.ReadFramebuffer(fb, 16);
uint P(int i) => (uint)(fb[i] | fb[i+1]<<8 | fb[i+2]<<16 | fb[i+3]<<24);
Check("framebuffer pixel 0", P(0)  == 0x11223344);
Check("framebuffer pixel 1", P(4)  == 0x55667788);
Check("framebuffer pixel 3", P(12) == 0xDEADBEEF);
// Present-from-RAM (fbaddr → shadow) reached the host PresentedPixels SDL reads.
var pres = emu.Framebuffer.PresentedPixels;
uint Q(int i) => (uint)(pres[i*4] | pres[i*4+1]<<8 | pres[i*4+2]<<16 | pres[i*4+3]<<24);
bool presOk = true;
foreach (int i in new[] { 0, 1, 100, 1000, 320*200 - 1 })
    if (Q(i) != (uint)(i * 7 + 0x100)) presOk = false;
Check("framebuffer presented (fbaddr→RAM)", presOk);

// Audio PCM payload (device) + control snapshot (reconciled to AudioControl).
var pcm = new byte[16]; emu.ReadPcm(pcm, 16);
bool pcmOk = true; for (int i = 0; i < 16; i++) if (pcm[i] != (byte)(0x40 + i)) pcmOk = false;
Check("audio PCM payload", pcmOk);
Check("audio rate 44100",  emu.AudioControl.SampleRate == 44100);
Check("audio channels 2",  emu.AudioControl.Channels == 2);
Check("audio buflen 16",   emu.AudioControl.BufLength == 16);
Check("audio playing",     emu.AudioControl.IsPlaying);

// MIDI note-on captured via the ring → OnMidi.
Check("midi message",      midi.Count == 1 && midi[0].off == 0x04 && midi[0].val == 0x00403C90);

// Vanilla RV32I: no M/A extensions to verify (the core traps them illegal).

Console.WriteLine($"\n{(fails == 0 ? "ALL PERIPHERAL CHECKS PASSED" : $"{fails} CHECK(S) FAILED")}  (exit {emu.ExitCode})");
return fails == 0 ? 0 : 1;

bool Clang(string[] args)
{
    var psi = new ProcessStartInfo(clang) { RedirectStandardError = true, UseShellExecute = false };
    foreach (var a in args) psi.ArgumentList.Add(a);
    var p = Process.Start(psi)!; string err = p.StandardError.ReadToEnd(); p.WaitForExit();
    if (p.ExitCode != 0) { Console.Error.WriteLine($"clang failed ({p.ExitCode})\n{err}"); return false; }
    return true;
}
