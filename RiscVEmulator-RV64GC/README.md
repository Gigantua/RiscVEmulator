# RV64GC RISC-V Emulator

> **A surprisingly small emulator that boots a real Alpine Linux distribution,
> runs a graphical desktop, plays Doom, and hosts a C compiler.**

The entire CPU is a single C++ file. The instruction decoder is a `switch`
statement. There is no JIT — just a clean RV64GC interpreter with an Sv39 MMU
and an SBI firmware shim. It boots **Alpine Linux `riscv64`**, runs a windowed
**IceWM/XFCE desktop** with sound and input, installs software with the real
**`apk`** package manager, and still has room to run TinyCC — a C compiler
compiling C *inside the emulator*.

This project demonstrates that **a simple, readable RISC-V implementation can
do remarkable things.**

![Alpine Linux](https://img.shields.io/badge/boots-Alpine%20Linux%20riscv64-0d597f) ![Desktop](https://img.shields.io/badge/runs-IceWM%20desktop-brightgreen) ![Doom](https://img.shields.io/badge/runs-Doom-red) ![TinyCC](https://img.shields.io/badge/compiles-C%20in%20TinyCC-blue) ![.NET 10](https://img.shields.io/badge/.NET-10-blueviolet)

---

## What it can do

| | |
|---|---|
| 🐧 **Alpine Linux `riscv64`** | Boots a genuine, unmodified Alpine `riscv64` userland from an initramfs, then installs itself to a persistent virtio-blk disk. Survives reboots like a real machine. |
| 🖥️ **Graphical desktop** | Boot with `--gui` for an **IceWM / XFCE** desktop in an SDL window — windows, taskbar, file manager, terminal, browser. Keyboard and mouse are forwarded into the guest as real Linux evdev devices. |
| 📦 **Real package manager** | `apk add ...` pulls any of the **~10 000 Alpine `riscv64` packages** straight from the official Alpine mirror over the emulated network. No hand-rolled feed — it is the same package manager Alpine ships. |
| 🔊 **Audio** | A PCM audio peripheral bridged to SDL on the host; the guest's `rvemu-rv64-audio` daemon streams `/dev/snd` audio out of the VM. |
| ⌨️ **Keyboard & mouse** | MMIO keyboard/mouse peripherals; the `rvemu-rv64-input` guest daemon synthesises Linux `input_event`s into `/dev/uinput`, so every app sees ordinary evdev devices. |
| 🛜 **Networking** | Host-loopback NAT through libslirp — DHCP, DNS, HTTP, `apk`, `wget`, all standard sockets. |
| 💾 **Persistent disk** | A 3 GiB virtio-blk image holds the installed Alpine system + desktop; deleting it triggers a fresh install on next boot. |
| 🎮 **Doom** | Full DOOM (PureDOOM) compiled to RV64GC at launch and run bare-metal at full speed. |
| ⚙️ **TinyCC** | A C compiler running inside the emulator, JIT-compiling C programs for the very CPU it runs on. |
| 🎬 **Video / 🌄 Voxel / 🎵 Sound** | Bare-metal graphics and audio demos written entirely in guest C. |

All of this from a single-file C++ CPU core and a thin C# peripheral layer.

```cpp
static void do_step(CPU_State& cpu) {
    if (check_interrupts(cpu)) { cpu.regs[0] = 0; return; }
    ...
    u64 pa0 = mmu_translate(cpu, cpu.pc, /*fetch*/0, ifault);   // Sv39 walk
    u32 lo  = mem_read<u16>(cpu, pa0);
    u32 instr;  u64 ilen;
    if ((lo & 3u) == 3u) {                       // 32-bit instruction
        instr = lo | ((u32)mem_read<u16>(cpu, pa2) << 16);  ilen = 4;
    } else {                                     // 16-bit compressed (C-ext)
        instr = decompress((u16)lo);             ilen = 2;
    }

    switch (instr & 0x7F) {
    case 0x37: cpu.regs[rd] = (u64)(i64)(i32)(instr & 0xFFFFF000u);  break;  // LUI
    case 0x17: cpu.regs[rd] = cpu.pc + (i64)(i32)(instr & ~0xFFFu);  break;  // AUIPC
    case 0x6F: cpu.regs[rd] = cpu.pc + ilen; nextpc = cpu.pc + j_imm(instr); break;  // JAL
    /* ... LOAD / STORE / OP-IMM / OP / AMO / OP-FP / W-ext / SYSTEM ... */
    }
    ...
}
```

---

## ISA Support

The native core (`Native/rv64gc_core.cpp`) implements the full **RV64GC**
envelope — the `G` shorthand is `IMAFD` + `Zicsr` + `Zifencei`, plus `C`:

| Extension | Status | Notes |
|-----------|--------|-------|
| **RV64I** | ✅ full | Base 64-bit integer ISA, including the `*W` 32-bit-result variants (`ADDIW`/`SLLIW`/`SRLIW`/`SRAIW`, `ADDW`/`SUBW`/`SLLW`/`SRLW`/`SRAW`), `LD`/`SD`/`LWU`, 6-bit shift amounts |
| **M** | ✅ full | `MUL`/`MULH[SU\|U]`/`DIV[U]`/`REM[U]` + the `W` variants (`MULW`/`DIVW`/`DIVUW`/`REMW`/`REMUW`); `MULH` via `__int128` |
| **A** | ✅ full | `LR`/`SC` + `AMO{SWAP,ADD,XOR,AND,OR,MIN[U],MAX[U]}` in both `.W` and `.D` widths |
| **F** | ✅ full | Single-precision float; values NaN-boxed in the 64-bit `f`-registers |
| **D** | ✅ full | Double-precision float; `FLD`/`FSD`, format-bit OP-FP, fused multiply-add |
| **C** | ✅ full | Compressed 16-bit encodings — decoded by expanding each to its 32-bit equivalent |
| **Zicsr** | ✅ full | Complete CSR access |
| **Zifencei** | ✅ | `FENCE` / `FENCE.I` are NOPs (single-hart, coherent host memory) |
| **Privileged M/S/U** | ✅ full | All three modes, trap delegation (`medeleg`/`mideleg`), `MRET`/`SRET`/`WFI`/`ECALL`/`EBREAK`, timer & external interrupts |
| **Sv39 MMU** | ✅ full | Three-level page-table walk with 1 GiB / 2 MiB superpages; 256-entry TLB; hardware A/D bits; page faults |
| **SBI firmware** | ✅ full | The emulator acts as the M-mode SBI firmware; the kernel boots directly in S-mode (Base / TIME / IPI / RFENCE / SRST / DBCN) |

The CPU boots in **M-mode** with all CSRs zero. `satp.MODE` is WARL-clamped to
`{Bare, Sv39}` so a kernel probing Sv57/Sv48 cleanly falls back to Sv39.

---

## Features

- **RV64GC core** — `IMAFDC` + `Zicsr`/`Zifencei`, single-file C++ interpreter
- **Sv39 MMU** — three-level walk, superpages, hardware A/D, 256-entry TLB
- **SBI firmware** — emulator is the M-mode firmware; Linux runs in S-mode
- **Peripherals** — UART 16550, PLIC, virtio-net, virtio-blk, framebuffer,
  keyboard, mouse, audio (PCM), MIDI, real-time clock, CLINT timer
- **Networking** — libslirp host-loopback NAT (`10.0.2.0/24`); DHCP/DNS/HTTP
- **SDL2 frontend** — hardware-accelerated framebuffer window via
  [Silk.NET.SDL](https://github.com/dotnet/Silk.NET), with host keyboard,
  mouse and audio wired through to the guest
- **ELF loader** — loads `PT_LOAD` segments from ELF64 binaries; also decodes
  the RISC-V flat `Image` header for raw kernels
- **Integration test suite** — compiles C → RV64GC ELF → runs → asserts

---

## Repository Layout

```
Core/                    C# emulator engine: P/Invoke shell, memory bus,
                         MMIO dispatcher, host VA reservation, ELF loader
Core/Peripherals/        UART, PLIC, virtio-net, virtio-blk, framebuffer,
                         keyboard, mouse, audio, MIDI, RTC, syscon, host-exit
Core/Networking/         libslirp NAT backend + Win32 host-loopback fallback
Native/                  C++ CPU hot path — rv64gc_core.cpp (single file,
                         ClangCL) + slirp_bridge
Frontend/                SDL2 window (rendering, input, audio) via Silk.NET.SDL
Examples/
  Linux/                 Boot Alpine riscv64 — Examples.Linux --rv64 [--gui]
  Linux.Build_RV64/      WSL-driven build: buildroot kernel, device tree,
                         Alpine initramfs + /init + guest daemons
  Linux.Packageserver/   Optional cross-build .ipk feed server
  Doom/                  Full Doom port (PureDOOM, compiles at launch)
  Runner/                Generic ELF runner with all peripherals wired
  TinyCC/                TinyCC C compiler running inside the emulator
  Video/ Voxel/ Sound/   Bare-metal graphics & audio demos
  Input/ Midi/ Mp4Player/
RiscVEmulator.Tests/     Integration tests (compile C → ELF → run → assert)
  Programs/              C test programs + linker.ld
  Runtime/               Bare-metal C runtime (libc, malloc, softfloat, VFS)
```

---

## Architecture

```
Emulator64 (C# P/Invoke shell)
  │  HostMemoryReservation → reserves the guest VA range up front
  │  passes the base pointer to native; both sides share the same memory
  │
  ├── rv64gc_core.dll  (C++ hot path — ClangCL)
  │     entire CPU state in one CPU_State struct; do_step(CPU_State&)
  │     every guest access is one *(volatile T*)(mem + paddr) dereference
  │     Sv39 translation + SBI firmware all live in native code
  │
  ├── MemoryBus + Peripherals
  │     each peripheral commits its guest slice as plain (RAM, framebuffer,
  │     audio PCM) or guarded PAGE_NOACCESS (MMIO registers)
  │
  ├── MmioDispatcher (Windows VEH)
  │     catches access violations on guarded pages, decodes the faulting
  │     x86-64 MOV, dispatches to IPeripheral.Read/Write, resumes
  │
  └── ElfLoader  ELF64 PT_LOAD segments / RISC-V flat Image header
```

The C# layer reserves the guest's physical address space on the host and
hands the base pointer to the native DLL, which dereferences `mem + paddr`
directly — no copies, no per-access dispatch. RAM and bulk-data peripherals
are plain committed pages; MMIO registers are guarded pages whose access
violations a vectored exception handler turns into `IPeripheral` calls.

---

## Memory Map

| Address | Size | Device |
|---------|------|--------|
| `0x0C000000` | 64 MB | PLIC (external-interrupt controller) |
| `0x10000000` | 256 B | UART 16550 (console I/O) |
| `0x10001000` | 256 B | Keyboard controller (scancode FIFO) |
| `0x10002000` | 256 B | Mouse controller (relative deltas + buttons) |
| `0x10003000` | 256 B | Real-Time Clock (wall-clock µs / ms / epoch) |
| `0x10005000` | 256 B | MIDI output |
| `0x10008000` | 4 KB | virtio-net (libslirp NAT) |
| `0x10009000` | 4 KB | virtio-blk (persistent disk) |
| `0x11100000` | 4 KB | SYSCON (reboot / poweroff) |
| `0x30000000` | 1 MB | Audio PCM buffer |
| `0x30100000` | 256 B | Audio control (sample rate, channels, play/stop) |
| `0x40000000` | — | Host-exit (write exit code → halt) |
| `0x80000000` | 1 GB | RAM |
| `0xBFC00000` | 4 MB | Framebuffer (1024×768 RGBA8888, top of RAM) |

See [MEMORY_MAP.md](MEMORY_MAP.md) for full register-level details.

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| [.NET 10 SDK](https://dotnet.microsoft.com/download) | Build and run the C# projects |
| Visual Studio 2022 / 2026 with **C++ workload + Clang/LLVM** | Build the native `rv64gc_core.dll` (ClangCL toolset) |
| [LLVM/Clang](https://releases.llvm.org/) in `PATH` | Cross-compile bare-metal RV64GC ELF guest programs |
| `lld` linker in `PATH` | Link bare-metal RV64GC ELF binaries (`-fuse-ld=lld`) |
| **WSL2 + Ubuntu** | `Examples.Linux.Build_RV64` drives buildroot in WSL to produce the kernel, device tree and Alpine initramfs |

> **Windows only** (host). Bare-metal demos work without WSL; building the
> Alpine Linux image needs it.

---

## Build

```powershell
# Full solution — builds the C++ DLL and all C# projects
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    RiscVEmulator.sln -p:Platform=x64
```

The native `rv64gc_core.dll` is automatically copied into every C# output
directory via `ProjectReference`.

---

## Examples

### Alpine Linux desktop

Build the boot image once (drives buildroot in WSL — kernel, device tree,
Alpine `riscv64` initramfs):

```powershell
dotnet run --no-build --project Examples\Linux.Build_RV64 -p:Platform=x64
```

Then boot. The first run installs Alpine + the IceWM/XFCE desktop onto a fresh
3 GiB virtio-blk disk; subsequent runs are instant and persistent:

```powershell
# Graphical desktop in an SDL window
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --rv64 --gui

# Serial console only
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --rv64
```

What you get:

- A genuine **Alpine `riscv64`** system — `apk`, `busybox`, musl, OpenRC.
- A graphical **IceWM / XFCE desktop** with taskbar, terminal, file manager
  and a web browser, rendered into the host SDL window.
- Working **keyboard, mouse and audio** — host input is forwarded into the
  guest as evdev devices; guest audio streams back out to SDL.
- **Networking** — DHCP on boot, then `apk add <anything>` pulls packages
  straight from the official Alpine mirror.
- A **persistent disk** — installed software and files survive reboots;
  delete `~/.cache/riscvemu/linux/rv64-disk.img` for a clean install.

Options: `--kernel <path>`, `--initrd <path>`, `--dtb <path>`,
`--auto-commands "<cmds>"` (inject shell commands non-interactively),
`--screenshot <file.png>`. Press **Ctrl+C** in the host shell to exit.

### Doom

The classic. Compiles `doom_main.c` (using
[PureDOOM](https://github.com/Daivuk/PureDOOM)) to RV64GC ELF at startup,
then runs it bare-metal with a real `doom1.wad`.

```powershell
dotnet run --no-build --project Examples\Doom -p:Platform=x64 -- [--wad path\to\doom.wad] [--scale 3]
```

### Runner

Generic ELF runner — wires up every peripheral and opens an SDL window.

```powershell
dotnet run --no-build --project Examples\Runner -p:Platform=x64 -- <elf-file> [--scale 3] [--ram 64]
```

### Other demos

| Example | Description |
|---------|-------------|
| `Examples.TinyCC` | [TinyCC](https://bellard.org/tcc/) running inside the emulator — a C compiler in C, JIT-compiling RV64 code |
| `Examples.Video` | Software video renderer — plays a raw frame sequence |
| `Examples.Voxel` | Voxel terrain with height-map rendering |
| `Examples.Sound` | PCM audio playback via the audio peripheral |
| `Examples.Midi` | MIDI output demo |
| `Examples.Input` | Keyboard and mouse event demo |

---

## Tests

```powershell
dotnet test --no-build RiscVEmulator.Tests
```

Tests compile small C programs with clang → RV64GC ELF, load them into the
emulator, run them, and assert on console output and exit code. The
`LinuxBootTest` suite additionally boots Alpine `riscv64` and runs `apk` to
verify the full distribution path. The bare-metal runtime (`Runtime/`)
provides libc, malloc, softfloat and a VFS shim.

**Requirements:** `clang` and `lld` for `riscv64-unknown-elf` in `PATH`.

---

## Writing Guest Programs

Guest programs are ordinary C compiled for bare-metal RV64GC:

```bash
clang --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d \
      -nostdlib -O3 -fuse-ld=lld \
      -T Runtime/linker.ld \
      my_program.c Runtime/runtime.c Runtime/libc.c \
      -o my_program.elf
```

The `--target` / `-march` / `-mabi` triple is centralised in
`Core/RiscVTarget.cs` — every compiler invocation in the solution derives
from it. The runtime provides `printf`/`puts`/`scanf`, `malloc`/`free`,
IEEE-754 soft-float helpers, a small VFS, and a syscall shim over `ECALL`.

---

## Companion Docs

| File | Contents |
|------|----------|
| `AGENTS.md` | LLM onboarding |
| `Architecture.md` | Full RISC-V ISA reference |
| `MEMORY_MAP.md` | Register-level peripheral details |
| `RV64_PLAYBOOK.md` | Booting a real distro — design notes |
| `ISA_PLAYBOOK.md` | End-to-end procedure for adding a CPU feature |
</content>
</invoke>
