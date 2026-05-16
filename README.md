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
| 🎮 **Doom** | Full DOOM running in RISCV c++ emulation layer. Two flavors: bare-metal PureDOOM (`Examples.Doom`) and a windowed `doomgeneric` build that runs inside the Linux desktop. |
| 🐧 **Linux 6.6 + desktop** | Real nommu kernel boots to a **graphical Microwindows desktop** with taskbar, terminal, clock, eyes, calc, chess, tetris, etc. Drag windows, click buttons, type into apps. |
| 💻 **Linux Terminal app** | A nano-X terminal-emulator window (`rvemu-term`) runs `sh -i` over a real Unix98 pty with full VT100 escape parsing — nano, vi, less, top all work. |
| 📦 **Self-hosted OPKG feed** | `Examples.Linux.Packageserver` drives buildroot to cross-compile any of ~2700 buildroot packages, wraps the output as `.ipk`, and serves them over HTTP. The guest's `rvpkg` installs them like a real package manager. |
| 🛜 **Networking** | Host-loopback NAT through libslirp — `wget`, DHCP, all standard sockets. |
| 🔊 **Audio + RTC + MIDI** | PCM, real-time clock, and MIDI output peripherals — wired into the bare-metal Doom and reachable from any guest userspace via `/dev/snd` (ALSA bridge). |
| ⚙️ **TinyCC** | A C compiler running inside the emulator, compiling C programs. |
| 🎬 **Video** | Software-rendered frame sequences at real-time speed. |
| 🌄 **Voxel terrain** | Height-map voxel renderer written entirely in guest C. |

All of this from ~550 lines of core C++ and a thin C# peripheral layer.

~~~cpp
static __forceinline void do_step(CPU_State& cpu)
static __forceinline void do_step() {
    if constexpr (Priv) {
        if (check_interrupts()) { regs[0] = 0; mtime++; return; }
        if (wfi_pending)        { regs[0] = 0; mtime++; return; }
    }

    const uint32_t instr  = mem_read<uint32_t>(pc);
    const uint32_t opcode = instr & 0x7F;
    const int      rd     = (instr >>  7) & 0x1F;
    const int      rs1    = (instr >> 15) & 0x1F;
    const int      rs2    = (instr >> 20) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const uint32_t f7     = (instr >> 25) & 0x7F;
    const int32_t  s1     = (int32_t)regs[rs1];
    const uint32_t u1     = regs[rs1];
    const int32_t  s2     = (int32_t)regs[rs2];
    const uint32_t u2     = regs[rs2];
    uint32_t nextpc       = pc + 4;
    uint32_t trap_cause   = 0;
    uint32_t trap_tval    = 0;

    switch (opcode) {

    case 0x37: regs[rd] = instr & 0xFFFFF000u;                           break;  // LUI
    case 0x17: regs[rd] = pc + (instr & 0xFFFFF000u);                    break;  // AUIPC
    case 0x6F: regs[rd] = pc + 4; nextpc = pc + j_imm(instr);            break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;               // JALR
                 regs[rd] = pc + 4; nextpc = t;                          break; }

    case 0x63: {  // BRANCH
        int taken;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
            default: taken = 0;
        }
        if (taken) nextpc = pc + b_imm(instr);
        break;
    }

    case 0x03: {  // LOAD
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: regs[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (addr); break;
            case 1: regs[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(addr); break;
            case 2: regs[rd] =                    mem_read<uint32_t>(addr); break;
            case 4: regs[rd] =                    mem_read<uint8_t> (addr); break;
            case 5: regs[rd] =                    mem_read<uint16_t>(addr); break;
        }
        break;
    }

    case 0x23: {  // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(addr,            u2); break;
        }
        break;
    }

    case 0x13: {  // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        uint32_t r;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                            break;
            case 1: r = u1 << sh;                                         break;
            case 2: r = s1 < imm           ? 1u : 0u;                    break;
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                    break;
            case 4: r = u1 ^ (uint32_t)imm;                              break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;    break;
            case 6: r = u1 | (uint32_t)imm;                              break;
            case 7: r = u1 & (uint32_t)imm;                              break;
            default: r = 0;
        }
        regs[rd] = r;
        break;
    }

    case 0x33: {  // OP
        uint32_t r;
        if (MExt && f7 == 0x01) {
            r = exec_m(f3, s1, u1, s2, u2);
        } else {
            const int sh = s2 & 0x1F;
            switch (f3) {
                case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
                case 1: r = u1 << sh;                                                break;
                case 2: r = s1 < s2 ? 1u : 0u;                                       break;
                case 3: r = u1 < u2 ? 1u : 0u;                                       break;
                case 4: r = u1 ^ u2;                                                  break;
                case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;            break;
                case 6: r = u1 | u2;                                                  break;
                case 7: r = u1 & u2;                                                  break;
                default: r = 0;
            }
        }
        regs[rd] = r;
        break;
    }

    case 0x2F:  // A-extension removed: LR/SC/AMO trap as illegal
        trap_cause = 2;
        trap_tval = instr;
        break;

    case 0x07: case 0x27:                  // FLW/FSW
    case 0x43: case 0x47: case 0x4B: case 0x4F: // FMA family
    case 0x53:                             // OP-FP
        trap_cause = 2;
        trap_tval = instr;
        break;

    case 0x0F: break;  // FENCE / FENCE.I — NOP

    case 0x73: {  // SYSTEM
        const uint32_t f3s = (instr >> 12) & 0x7;
        trap_cause = exec_system<Priv>(instr, rd, f3s, nextpc, trap_tval);
        break;
    }

    }  // switch (opcode)

    if constexpr (Priv) {
        if (trap_cause) { do_trap(trap_cause, trap_tval); regs[0] = 0; mtime++; return; }
    }

    regs[0] = 0;
    pc = nextpc;
    if constexpr (Priv) { if (priv_mode == 0) umode_count++; }
    mtime++;
}
~~~

---

## Features

- **RV32I** — all 40 base instructions
- **No M-extension** — `MUL` / `MULH` / `DIV` / `REM` trap; guest code uses libcalls
- **No A-extension** — `LR.W`, `SC.W`, and `AMO*` trap as illegal instructions
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
  Doom/                  Full Doom port (PureDOOM, compiles at launch, bare metal)
  Linux/                 Boot Linux 6.6 nommu kernel + nano-X desktop (--gui)
  Linux.Build_RV32i/     WSL-driven buildroot prepare: toolchain, kernel,
                         busybox, Microwindows nano-X, rvemu-{input,taskbar,term},
                         doomgeneric (windowed), etc. → packed initramfs
  Linux.Packageserver/   Interactive REPL: pick from ~2700 buildroot packages,
                         cross-build, wrap as .ipk, serve OPKG feed at :8080
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
| Visual Studio 2022 / 2026 with **C++ workload + Clang/LLVM** | Build native `rv32i_core.dll` (ClangCL toolset) |
| [LLVM/Clang](https://releases.llvm.org/) in `PATH` | Cross-compile bare-metal RV32I ELF guest programs |
| `lld` linker in `PATH` | Link bare-metal RV32I ELF binaries (`-fuse-ld=lld`) |
| **WSL2 + Ubuntu** (for the Linux desktop) | `Examples.Linux.Build_RV32i` cross-compiles a nommu-uClibc rootfs + Microwindows desktop + ~30 packages via buildroot inside WSL |

> **Windows only** (host). Bare-metal demos work without WSL; the Linux desktop / package feed needs it.

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

### Linux desktop

Boots Linux 6.6 (nommu, uClibc) to a **graphical Microwindows nano-X desktop**.
Run with `--gui` to get an SDL window showing the framebuffer + a taskbar:

```powershell
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --gui
```

What you get out of the box (everything is a real nano-X client):

- **Terminal** — `rvemu-term`, a Microwindows window running `sh -i` over a Unix98 pty. Sets `TERM=vt100` so ncurses apps work; full VT100 escape parsing (cursor motion, ED/EL, SGR). Backspace, line editing, Ctrl-C all work.
- **Doom** — `doomgeneric` ported to render into a nano-X window via `GrArea()`. Movable, focusable, close-box. Plays the shareware WAD.
- **Clock / Eyes / Chess / Tetris / World / Mine / Calculator / Hello** — standard Microwindows demos. Taskbar reads `/etc/rvemu-launchers.d/*.desktop` and dynamically picks up new entries — install a package, click the new button within a second.
- **Mouse capture** — Esc releases pointer grab; click back in the window to re-capture. Keyboard layout-aware (German/French/Dvorak all type the same Linux KEY_* as US).
- **Window manager** — nanowm; drag titlebars, alt-tab between apps.

Quick build (needs WSL2 + Ubuntu — the prepare drives buildroot in WSL):

```powershell
# First-time setup builds buildroot toolchain, kernel, busybox, nano-X, taskbar (~30-40 min)
dotnet run --no-build --project Examples\Linux.Build_RV32i -p:Platform=x64
# Then boot:
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --gui
```

Log in as **root** (no password) on the serial console too. Press **Ctrl+C** in the host shell to exit the emulator.

Options:
```
--kernel <path>   Use a custom kernel flat binary
--dtb    <path>   Use a custom DTB
--ram    <MB>     Guest RAM in MB (default: 96; 96 minimum for the desktop)
--gui             Open the SDL framebuffer window
--download        Fetch the legacy pre-built mini-rv32ima kernel (serial-only mode)
```

The Build_RV32i kernel is built for `rv32i` and labels its boot banner with
`-rv32i`; a `mini-rv32ima` banner means you are running the legacy downloaded
image or a stale cache. The kernel and DTB are cached in
`~/.cache/riscvemu/linux/`.

### Self-hosted package feed

`Examples.Linux.Packageserver` is an interactive REPL that drives buildroot to cross-build any of buildroot's ~2700 packages and serves the results as an opkg-compatible `.ipk` feed over HTTP at `http://localhost:8080` (reachable from the guest as `http://10.0.2.2:8080` via libslirp NAT).

Host:
```
> search nano                    # find packages buildroot knows about
> add nano bc dropbear           # pick what you want
> build                          # cross-build, only un-cached packages
> rebuild doomgeneric            # force rebuild a cached package
> run                            # build + serve feed-cache/
```

Guest, in **Terminal**:
```
rvpkg update                     # pull Packages index from the host
rvpkg list                       # see what's available
rvpkg install nano               # download .ipk + extract to /
```

`rvpkg` is a 30-line POSIX-shell installer (no opkg/dpkg — those need MMU + wchar). If a package ships a `/etc/rvemu-launchers.d/*.desktop` file, the desktop's taskbar adds a button for it within ~1 second of install.

Caveats: packages calling `dlopen` (SDL2, anything plugin-loading), or `fork()` directly (vfork is fine) won't build for nommu+uClibc. Pure-C CLI tools (bc, nano, less, vim, dropbear, lynx) work out of the box. ~30 packages from the curated catalog have been verified.

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
| M | ❌ | MUL/DIV/REM opcodes trap; use integer libcalls |
| A | ❌ | LR/SC/AMO opcodes trap |
| F / D (float) | ❌ hardware | F/D opcodes trap; use `softfloat.c` in guest |
| Zicsr | ✅ | |
| M/S/U privilege | ✅ | Always enabled |
| Interrupts | ✅ | Timer interrupt via CLINT |
| FENCE | NOP | |
| ECALL/EBREAK | traps | |

---

## Syscalls (ECALL)

| a7 | Name | Behavior |
|----|------|----------|
| 64 | `write` | Output bytes to `UartDevice.OutputHandler` |
| 93 | `exit` | Halt emulator, set `ExitCode = a0` |
| 94 | `exit_group` | Same as `exit` |

Other syscall numbers are silently ignored. The runtime in `Runtime/syscalls.c` maps additional POSIX calls (time, file I/O via VFS) onto these.
