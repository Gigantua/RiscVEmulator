using System.Diagnostics;
using System.Runtime.InteropServices;
using RiscVEmulator.Core;
using RiscVEmulator.Core.Peripherals;
using RiscVEmulator.Frontend;

// ── Configuration ─────────────────────────────────────────────────
const uint StackPointer = 0x009FFF00;
const int RamMB = 32;

const int VideoWidth = 320;
const int VideoHeight = 200;
const int VideoFps = 24;
const int AudioRate = 22050;
const int AudioChannels = 1;

string clang = @"C:\Program Files\LLVM\bin\clang.exe";
string ffmpeg = "ffmpeg";

string exeDir = AppContext.BaseDirectory;

string solutionRoot = exeDir;
for (int depth = 0; depth < 10; depth++)
{
    string candidate = Path.GetFullPath(Path.Combine(exeDir, string.Join(Path.DirectorySeparatorChar.ToString(), Enumerable.Repeat("..", depth))));
    if (File.Exists(Path.Combine(candidate, "RiscVEmulator.sln")))
    {
        solutionRoot = candidate;
        break;
    }
}
string runtimeDir = Path.Combine(solutionRoot, "Runtime");
string linkerLd = Path.Combine(runtimeDir, "linker.ld");

string playerC = Path.Combine(exeDir, "Programs", "mp4_player.c");
string runtimeC = Path.Combine(runtimeDir, "runtime.c");
string libcC = Path.Combine(runtimeDir, "libc.c");
string syscallsC = Path.Combine(runtimeDir, "syscalls.c");
string mallocC = Path.Combine(runtimeDir, "malloc.c");

string buildDir = Path.Combine(exeDir, "build");
Directory.CreateDirectory(buildDir);

// ── Parse CLI args ────────────────────────────────────────────────
string? inputFile = null;
var opts = new SdlWindowOptions { Title = "Video Player — RV32I", GrabMouse = false, Scale = 3, TargetFps = 60 };

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--scale": opts.Scale = int.Parse(args[++i]); break;
        case "--fps": opts.TargetFps = int.Parse(args[++i]); break;
        default:
            if (!args[i].StartsWith("--")) inputFile = args[i];
            break;
    }
}

inputFile ??= Path.Combine(solutionRoot, "..", "big-buck-bunny-480p-30sec.mp4");

if (!File.Exists(inputFile))
{
    Console.Error.WriteLine($"Input video file not found: {inputFile}");
    Console.Error.WriteLine("Usage: dotnet run --project Examples\\Mp4Player [video.mp4]");
    return 1;
}

// ── Encode video to SVID format ──────────────────────────────────
string svidPath = Path.Combine(buildDir, "video.svid");

if (!File.Exists(svidPath) || File.GetLastWriteTime(inputFile) > File.GetLastWriteTime(svidPath))
{
    Console.WriteLine($"Encoding SVID: {Path.GetFileName(inputFile)}...");
    Console.WriteLine($"  {VideoWidth}×{VideoHeight} @ {VideoFps}fps, audio {AudioRate}Hz {AudioChannels}ch");

    // Extract raw RGBA frames
    string rawVideoPath = Path.Combine(buildDir, "raw_video.bin");
    string rawAudioPath = Path.Combine(buildDir, "raw_audio.bin");

    // Video extraction
    if (!RunFfmpeg("-y", "-i", inputFile,
        "-vf", $"scale={VideoWidth}:{VideoHeight}:force_original_aspect_ratio=decrease,pad={VideoWidth}:{VideoHeight}:(ow-iw)/2:(oh-ih)/2",
        "-r", VideoFps.ToString(),
        "-pix_fmt", "rgba", "-f", "rawvideo", rawVideoPath))
        return 1;

    // Audio extraction
    if (!RunFfmpeg("-y", "-i", inputFile,
        "-ar", AudioRate.ToString(), "-ac", AudioChannels.ToString(),
        "-f", "s16le", "-acodec", "pcm_s16le", rawAudioPath))
        return 1;

    byte[] rawVideo = File.ReadAllBytes(rawVideoPath);
    byte[] rawAudio = File.ReadAllBytes(rawAudioPath);

    int pixels = VideoWidth * VideoHeight;
    int frameBytes = pixels * 4;
    int numFrames = rawVideo.Length / frameBytes;
    int audioBytesPerFrame = (AudioRate * AudioChannels * 2) / VideoFps;

    Console.WriteLine($"  {numFrames} frames, audio {rawAudio.Length:N0} bytes");

    // Raw frame encoding — each frame is independently decodable (no delta)
    var frameDataBlobs = new List<byte[]>();
    var frameTable = new List<(uint offset, uint videoSize, uint audioSize)>();
    uint currentOffset = 0;

    for (int f = 0; f < numFrames; f++)
    {
        // Raw RGBA pixels for this frame
        byte[] videoBlob = new byte[frameBytes];
        Buffer.BlockCopy(rawVideo, f * frameBytes, videoBlob, 0, frameBytes);

        // Audio chunk for this frame
        int audioStart = f * audioBytesPerFrame;
        int audioLen = Math.Min(audioBytesPerFrame, rawAudio.Length - audioStart);
        if (audioLen < 0) audioLen = 0;
        byte[] audioBlob = audioLen > 0 ? rawAudio[audioStart..(audioStart + audioLen)] : Array.Empty<byte>();

        // Combined frame data: video then audio
        byte[] combined = new byte[videoBlob.Length + audioBlob.Length];
        Buffer.BlockCopy(videoBlob, 0, combined, 0, videoBlob.Length);
        Buffer.BlockCopy(audioBlob, 0, combined, videoBlob.Length, audioBlob.Length);

        frameTable.Add((currentOffset, (uint)videoBlob.Length, (uint)audioBlob.Length));
        frameDataBlobs.Add(combined);
        currentOffset += (uint)combined.Length;
    }

    // Write SVID file
    using var fs = new FileStream(svidPath, FileMode.Create);
    using var bw = new BinaryWriter(fs);

    // Header (24 bytes)
    bw.Write(0x44495653u); // magic "SVID"
    bw.Write((ushort)VideoWidth);
    bw.Write((ushort)VideoHeight);
    bw.Write((ushort)VideoFps);
    bw.Write((ushort)AudioRate);
    bw.Write((ushort)AudioChannels);
    bw.Write((ushort)numFrames);
    bw.Write(0u); // reserved1
    bw.Write(0u); // reserved2

    // Frame table
    foreach (var (off, vs, @as) in frameTable)
    {
        bw.Write(off);
        bw.Write(vs);
        bw.Write(@as);
    }

    // Frame data
    foreach (var blob in frameDataBlobs)
        bw.Write(blob);

    Console.WriteLine($"  video.svid: {fs.Length:N0} bytes");

    // Cleanup temp files
    File.Delete(rawVideoPath);
    File.Delete(rawAudioPath);
}
else
{
    Console.WriteLine($"Using cached video.svid ({new FileInfo(svidPath).Length:N0} bytes)");
}

byte[] svidData = File.ReadAllBytes(svidPath);

// ── Compile ──────────────────────────────────────────────────────
Console.WriteLine("Compiling video player for RV32IM...");

string softfloatC = Path.Combine(runtimeDir, "softfloat.c");
string[] cSources = { runtimeC, libcC, softfloatC, syscallsC, mallocC };
var objFiles = new List<string>();

foreach (string src in cSources)
{
    if (!File.Exists(src))
    {
        Console.Error.WriteLine($"Source not found: {src}");
        return 1;
    }
    string objName = Path.GetFileNameWithoutExtension(src) + ".o";
    string objPath = Path.Combine(buildDir, objName);
    if (!Compile(src, objPath, [$"-I{runtimeDir}"]))
        return 1;
    objFiles.Add(objPath);
}

string programsDir = Path.Combine(exeDir, "Programs");
{
    string playerObj = Path.Combine(buildDir, "mp4_player.o");
    if (!Compile(playerC, playerObj, [$"-I{programsDir}", $"-I{runtimeDir}"]))
        return 1;
    objFiles.Add(playerObj);
}

// Link
{
    var linkArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3", "-fno-builtin", "-fsigned-char",
        "-fuse-ld=lld", $"-Wl,-T,{linkerLd}",
    };
    linkArgs.AddRange(objFiles);
    string elfPath = Path.Combine(buildDir, "mp4_player.elf");
    linkArgs.AddRange(["-o", elfPath]);

    Console.Write("  Linking... ");
    if (!RunClang(linkArgs.ToArray()))
        return 1;
    Console.WriteLine("OK");
    Console.WriteLine($"  mp4_player.elf: {new FileInfo(elfPath).Length:N0} bytes");
}

string elf = Path.Combine(buildDir, "mp4_player.elf");

// ── Build SoC ────────────────────────────────────────────────────
Console.WriteLine("Building SoC...");
byte[] elfData = File.ReadAllBytes(elf);
var memory = new Memory(RamMB * 1024 * 1024);
var bus = new MemoryBus(memory);
var uart = new UartDevice();
var fb = new FramebufferDevice();
var display = new DisplayControlDevice(fb);
display.SetMemory(memory);
var kbd = new KeyboardDevice();
var mouse = new MouseDevice();
var rtc = new RealTimeClockDevice();
var audioBuf = new AudioBufferDevice();
var audioCtrl = new AudioControlDevice();
var disk = new DiskDevice();

bus.RegisterPeripheral(uart);
bus.RegisterPeripheral(fb);
bus.RegisterPeripheral(display);
bus.RegisterPeripheral(kbd);
bus.RegisterPeripheral(mouse);
bus.RegisterPeripheral(rtc);
bus.RegisterPeripheral(audioBuf);
bus.RegisterPeripheral(audioCtrl);
bus.RegisterPeripheral(disk);

uart.OutputHandler = c => Console.Write(c);

// Load SVID into disk device (streamed, not in guest RAM)
disk.LoadFile(svidData, bus);

var regs = new RegisterFile();
uint entry = ElfLoader.Load(elfData, bus);
regs.Write(2, StackPointer);

var emu = new Emulator(bus, regs, entry);
emu.EnableMExtension = true;
emu.OutputHandler = c => Console.Write(c);

Console.WriteLine($"  SVID on disk: {svidData.Length:N0} bytes ({svidData.Length / 1024 / 1024}MB)");

// ── Run ──────────────────────────────────────────────────────────
Console.WriteLine("Starting video player...");
var window = new SdlWindow(fb, display, kbd, mouse, audioBuf, audioCtrl, emu, opts);
return window.Run();

// ── Helpers ──────────────────────────────────────────────────────
bool Compile(string src, string obj, string[] extraFlags)
{
    string name = Path.GetFileName(src);
    Console.Write($"  {name}... ");

    var compileArgs = new List<string>
    {
        "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
        "-nostdlib", "-nostartfiles", "-O3",
        "-fno-builtin", "-fsigned-char", "-c",
    };
    compileArgs.AddRange(extraFlags);
    compileArgs.Add(src);
    compileArgs.Add("-o");
    compileArgs.Add(obj);

    if (!RunClang(compileArgs.ToArray()))
        return false;
    Console.WriteLine("OK");
    return true;
}

bool RunClang(string[] clangArgs)
{
    var psi = new ProcessStartInfo(clang)
    {
        RedirectStandardError = true,
        RedirectStandardOutput = true,
        UseShellExecute = false,
    };
    foreach (var a in clangArgs)
        psi.ArgumentList.Add(a);

    var proc = Process.Start(psi)!;
    string stderr = proc.StandardError.ReadToEnd();
    proc.WaitForExit();

    if (proc.ExitCode != 0)
    {
        Console.Error.WriteLine($"FAILED (exit {proc.ExitCode})");
        Console.Error.WriteLine(stderr);
        return false;
    }
    return true;
}

bool RunFfmpeg(params string[] ffmpegArgs)
{
    var psi = new ProcessStartInfo(ffmpeg)
    {
        RedirectStandardError = true,
        RedirectStandardOutput = true,
        UseShellExecute = false,
    };
    foreach (var a in ffmpegArgs)
        psi.ArgumentList.Add(a);

    Console.Write("  ffmpeg... ");
    var proc = Process.Start(psi)!;
    string stderr = proc.StandardError.ReadToEnd();
    proc.WaitForExit();

    if (proc.ExitCode != 0)
    {
        Console.Error.WriteLine("FAILED");
        Console.Error.WriteLine(stderr);
        return false;
    }
    Console.WriteLine("OK");
    return true;
}
