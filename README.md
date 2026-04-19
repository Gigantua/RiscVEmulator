# RV32I RISC-V Emulator

> **A surprisingly small emulator that runs Linux, Doom, and a C compiler.**

The entire CPU is a single C++ file. The instruction decoder is a `switch` statement.
There are no JIT, no MMU, no page tables. Plays Doom at full speed,
and runs TinyCC — a C compiler compiling C *inside the emulator*.
Boots Linux 6.1

This project demonstrates that **a simple, readable RISC-V implementation can do remarkable things.**

![Doom](https://img.shields.io/badge/runs-Doom-red) ![Linux 6.1](https://img.shields.io/badge/boots-Linux%206.1-brightgreen) ![TinyCC](https://img.shields.io/badge/compiles-C%20in%20TinyCC-blue) ![.NET 10](https://img.shields.io/badge/.NET-10-blueviolet)

---

## What it can do

| | |
|---|---|
| 🎮 **Doom** | Full DOOM running in RISCV c++ emulation layer. Compiles from source and runs at launch. |
| 🐧 **Linux 6.1** | Real nommu kernel boots to an interactive BusyBox shell. |
| ⚙️ **TinyCC** | A C compiler running inside the emulator, compiling C programs. |
| 🎬 **Video** | Software-rendered frame sequences at real-time speed. |
| 🌄 **Voxel terrain** | Height-map voxel renderer written entirely in guest C. |
| 🔊 **Audio** | PCM playback through the emulated sound peripheral. |

All of this from ~1 000 lines of C++ and a thin C# peripheral layer.

---

## Features

- **RV32I** — all 40 base instructions
- **M-extension** — `MUL` / `MULH` / `DIV` / `REM` family (opt-in, big speedup for Doom)
- **A-extension** — atomic instructions (`LR.W`, `SC.W`, `AMO*`) needed for Linux SMP primitives
- **M/S/U privilege modes** — CSRs, traps, `MRET`/`SRET`, timer interrupts, `WFI`
- **Memory-mapped peripherals** — UART, framebuffer, keyboard, mouse, audio, RTC, CLINT
- **ELF loader** — loads `PT_LOAD` segments from standard ELF32 binaries
- **SDL2 frontend** — hardware-accelerated window at ~120 fps via [Silk.NET.SDL](https://github.com/dotnet/Silk.NET)
- **Bare-metal C runtime** — libc, malloc, softfloat, VFS, syscall shim for writing guest programs in C
- **Integration test suite** — compiles C programs to RV32I ELF and asserts on output

---

## Repository Layout

```
Core/                    C# emulator engine (P/Invoke shell, memory bus, peripherals)
Native/                  C++ CPU hot path (single-file, ClangCL vcxproj)
Frontend/                SDL2 window (rendering, input, audio) via Silk.NET.SDL
Examples/
  Doom/                  Full Doom port (PureDOOM, compiles at launch)
  Linux/                 Boot Linux 6.1 nommu kernel to an interactive shell
  Runner/                Generic ELF runner with all peripherals wired
  Video/                 Software-rendered video playback demo
  Voxel/                 Voxel terrain renderer demo
  Sound/                 PCM audio playback demo
  Input/                 Keyboard and mouse input demo
  TinyCC/                TinyCC C compiler running inside the emulator
RiscVEmulator.Tests/     Integration tests (compile C → ELF → run → assert)
  Programs/              C test programs + linker.ld
  Runtime/               Bare-metal C library (libc, malloc, softfloat, syscalls, VFS)
```

---

## Architecture

```
Emulator (C# P/Invoke shell)
  │  pins Memory.Data[] → passes IntPtr to native
  │
  ├── rv32i_core.dll  (C++ hot path — ClangCL)
  │     CPU registers, PC, CSRs, mtime — all live in native
  │     step loop runs entirely in C++
  │     MMIO / ECALL → callbacks into C#
  │
  ├── MemoryBus        Routes MMIO by address range → peripheral
  │     ├── Memory     Pinned byte-array RAM (shared with C++, zero-copy)
  │     └── IPeripheral[]  UART, timer, framebuffer, keyboard, mouse, audio, RTC
  │
  └── ElfLoader        Loads PT_LOAD segments, returns entry point
```

The C# layer allocates RAM and pins it with `GCHandle`. The native DLL receives an `IntPtr` and reads/writes directly — no copies. MMIO accesses above the RAM ceiling call back into C#, which routes them through `MemoryBus` to the appropriate peripheral.

---

## Memory Map

| Address | Size | Device |
|---------|------|--------|
| `0x00000000` | 16 MB (configurable) | RAM |
| `0x02000000` | 64 KB | CLINT Timer (standard SiFive layout) |
| `0x10000000` | 256 B | UART 16550 (console I/O) |
| `0x10001000` | 256 B | Keyboard controller (scancode FIFO) |
| `0x10002000` | 256 B | Mouse controller (relative deltas + buttons) |
| `0x10003000` | 256 B | Real-Time Clock (wall-clock µs / ms / epoch) |
| `0x20000000` | 256 KB | Framebuffer (320×200 RGBA8888) |
| `0x20100000` | 256 B | Display control (resolution, vsync, palette) |
| `0x30000000` | 1 MB | Audio PCM buffer |
| `0x30100000` | 256 B | Audio control (sample rate, channels, play/stop) |

See [MEMORY_MAP.md](MEMORY_MAP.md) for full register-level details.

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| [.NET 10 SDK](https://dotnet.microsoft.com/download) | Build and run C# projects |
| Visual Studio 2022 with **C++ workload + Clang/LLVM** | Build native `rv32i_core.dll` (ClangCL toolset) |
| [LLVM/Clang](https://releases.llvm.org/) in `PATH` | Cross-compile guest programs to RV32I ELF |
| `lld` linker in `PATH` | Link RV32I ELF binaries (`-fuse-ld=lld`) |

> **Windows only.** The native project uses the ClangCL toolset and builds a `.dll`.

---

## Build

```powershell
# Full solution — builds C++ DLL and all C# projects
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln
```

The native `rv32i_core.dll` is automatically copied to every C# output directory via `ProjectReference`.

---

## Examples

### Doom

The classic. Compiles `doom_main.c` (using [PureDOOM](https://github.com/Daivuk/PureDOOM)) to RV32I ELF at startup, then runs it with a real `doom1.wad`.

```powershell
cd Examples\Doom\bin\Debug\net10.0
dotnet Examples.Doom.dll [--wad path\to\doom.wad] [--scale 3] [--no-grab]
```

- Mouse is grabbed by default; press **Escape** or **Alt+F4** to exit.
- `--no-m-ext` disables the M-extension (much slower — avoid unless testing).

### Linux

Boots Linux 6.1.14 (nommu, Buildroot) to an interactive shell.
Downloads a ~10 MB pre-built kernel image on first run.

```powershell
cd Examples\Linux\bin\Debug\net10.0
dotnet Examples.Linux.dll --download   # first run: fetch kernel + DTB
dotnet Examples.Linux.dll              # subsequent runs use cache
```

Log in as **root** (no password). Press **Ctrl+C** to exit the emulator (the signal is *not* forwarded to the guest).

Options:
```
--kernel <path>   Use a custom kernel flat binary
--dtb    <path>   Use a custom DTB
--ram    <MB>     Guest RAM in MB (default: 128)
--download        Download and cache the pre-built kernel image
```

The kernel and DTB are cached in `~/.cache/riscvemu/linux/`.

### Runner

Generic ELF runner. Wires up all peripherals and opens an SDL window.

```powershell
dotnet Examples.Runner.dll <elf-file> [--scale 3] [--ram 16] [--m-ext] [--load <file> <hex-addr>]
```

### Other demos

| Example | Description |
|---------|-------------|
| `Examples.Video` | Software video renderer — plays a raw frame sequence |
| `Examples.Voxel` | Voxel terrain with height-map rendering |
| `Examples.Sound` | PCM audio playback via the audio peripheral |
| `Examples.Input` | Keyboard and mouse event demo |
| `Examples.TinyCC` | [TinyCC](https://bellard.org/tcc/) running inside the emulator — a C compiler in C |

---

## Tests

```powershell
dotnet test --no-build RiscVEmulator.Tests
```

Tests compile small C programs with clang → RV32I ELF, load them into the emulator, run them, and assert on console output and exit code. The bare-metal runtime (`Runtime/`) provides libc, malloc, softfloat, and a VFS shim.

**Requirements:** `clang` and `lld` for `riscv32-unknown-elf` must be in `PATH`.

---

## Writing Guest Programs

Guest programs are ordinary C compiled for bare-metal RV32I:

```bash
clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
      -nostdlib -O3 -fuse-ld=lld \
      -T RiscVEmulator.Tests/Programs/linker.ld \
      my_program.c RiscVEmulator.Tests/Runtime/runtime.c \
      RiscVEmulator.Tests/Runtime/libc.c \
      -o my_program.elf
```

The runtime provides:
- `printf` / `puts` / `scanf` / string functions (via UART MMIO)
- `malloc` / `free` (heap grows upward from BSS end)
- Soft-float and soft-double (IEEE 754 in software)
- VFS with `open` / `read` / `write` / `lseek`
- Syscall shim (`exit`, `write`) over `ECALL`

Default memory layout:
```
0x00001000   ELF entry point
     ↓       .text / .rodata / .data / .bss
     ↓       heap (grows up)
     ↑       stack (grows down)
0x009FFF00   initial stack pointer
```

---

## ISA Support

| Extension | Status | Notes |
|-----------|--------|-------|
| RV32I | ✅ All 40 instructions | |
| M | ✅ opt-in | `Emulator.EnableMExtension = true` |
| A | ✅ opt-in | `Emulator.EnableAExtension = true` (required for Linux) |
| Zicsr | ✅ | When `EnablePrivMode = true` |
| M/S/U privilege | ✅ opt-in | `Emulator.EnablePrivMode = true` (required for Linux) |
| F / D (float) | ❌ hardware | Use `softfloat.c` in guest |
| Interrupts | ✅ | Timer interrupt via CLINT; requires `EnablePrivMode` |
| FENCE | NOP | |
| EBREAK | halts CPU | |

---

## Syscalls (ECALL)

| a7 | Name | Behavior |
|----|------|----------|
| 64 | `write` | Output bytes to `UartDevice.OutputHandler` |
| 93 | `exit` | Halt emulator, set `ExitCode = a0` |
| 94 | `exit_group` | Same as `exit` |

Other syscall numbers are silently ignored. The runtime in `Runtime/syscalls.c` maps additional POSIX calls (time, file I/O via VFS) onto these.
