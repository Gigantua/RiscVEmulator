# AGENTS.md — RV32I RISC-V Emulator

Onboarding for LLM agents. Read this once and you can navigate the codebase.
For deeper docs see `README.md`, `Architecture.md` (ISA reference), `MEMORY_MAP.md`
(per-peripheral register layout), and `CLAUDE.md` (overlaps with this file).

## What this is

A RISC-V RV32I emulator (single-hart, no A/M/F/D, no C-ext) targeting Windows.
Hot path is a single-file C++ CPU compiled with ClangCL; everything else
(peripherals, frontend, examples) is .NET 10 C#. Runs bare-metal ELF binaries
and a real RV32I Linux kernel (`Examples/Linux`, built by
`Examples/Linux.Build_RV32i`).

## The one idea you must internalize

> The CPU has no concept of peripherals. Every guest load/store is one line:
> `*(volatile T*)(cpu.mem + addr)`.

How that works:

1. `HostMemoryReservation` reserves a 4 GB VA range on the host (`VirtualAlloc`
   with `MEM_RESERVE | PAGE_NOACCESS`). Reserved pages cost only a VAD entry.
2. For each peripheral, the `Emulator` commits its guest-address slice as
   either `PAGE_READWRITE` (plain memory: RAM, framebuffer, audio PCM) or
   `PAGE_NOACCESS` (guarded MMIO: UART, keyboard, CLINT, etc.).
3. The C++ CPU dereferences `cpu.mem + guest_addr` directly. Plain pages just
   work. Guarded pages raise an access violation.
4. `MmioDispatcher` is a process-wide Windows vectored exception handler. It
   catches the AV, looks up which peripheral owns the page, decodes the
   x86-64 MOV at the faulting RIP, calls `IPeripheral.Read`/`Write`, writes
   the result back into the saved register context, advances RIP past the
   instruction. The CPU never knows anything happened.

This is why `do_step()` has zero MMIO branches — every access is `*(mem + addr)`.

## Solution layout

```
Native/          C++ CPU hot path. rv32i_core.cpp (~355 lines), ClangCL vcxproj.
                 Read the file header for ISA support summary.
Core/            Emulator shell: P/Invoke wrappers, memory bus, peripherals,
                 ELF loader, register file, MMIO dispatcher.
Core/Peripherals Concrete IPeripheral implementations (12 devices).
Frontend/        SDL2 window: rendering, keyboard/mouse input, audio
                 (Silk.NET.SDL bindings).
Examples/        Demo apps — see "Examples" below.
RiscVEmulator.Tests/   Integration tests: compile C → ELF → run → assert.
RiscVEmulator.Tests/Runtime/  Bare-metal C runtime (libc, malloc, softfloat,
                              syscalls, vfs) linked into test programs.
```

## Build and run

```powershell
# Full solution (C++ via ClangCL + all C# projects). Requires VS C++ workload
# with the Clang toolset, plus clang+lld for riscv32 in PATH for tests.
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln -p:Platform=x64

# Just the native DLL
& "...MSBuild.exe" Native\rv32i_core.vcxproj -p:Platform=x64 -p:Configuration=Release

# Tests
dotnet test --no-build RiscVEmulator.Tests

# Run an example
dotnet run --no-build --project Examples\Doom -p:Platform=x64
```

The native `rv32i_core.dll` builds with `IgnoreAllDefaultLibraries=true` — there
is no CRT. The file provides its own `memset` and `memcpy`. Do not add
`<cstring>`, `<cmath>`, or any header that introduces libc dependencies.

## CPU model (Native/rv32i_core.cpp)

- The file is two blocks (see the file header): a **base RV32I CPU** and a
  **trap unit** wrapped around it.
- CPU state is one struct: `CPU_State` (`regs[32]`, `pc`, `halted`, `mem`
  pointer). The trap unit keeps its own `TrapUnit` struct — just current
  privilege and the host interrupt-pin latch. There are **no CSRs**.
- `cpu_step(CPU_State&)` executes one base instruction; for any SYSTEM opcode
  or non-RV32I encoding it returns a `CpuException` and never acts on it.
  `do_step()` drives one step — sample interrupts, handle the trap-return
  gateway, else `cpu_step` and let the trap unit handle anything raised. Single
  globals `static CPU_State cpu;` / `TrapUnit trap;` exist for the C-ABI
  trampolines (`rv32i_step_n`, `rv32i_init`, `rv32i_get_pc`, etc.).
- Privileged mode is **always on**. The CPU boots in M-mode (`trap.priv =
  PRIV_M`). Bare-metal programs that don't ECALL/EBREAK are unaffected; they
  exit via MMIO write to `0x40000000` (`HostExitDevice`).
- Trap state lives in the **trap-frame page** at guest-physical `0x0F000000`
  (`TrapFrameDevice`), plain RAM: interrupt-enable flag, per-source mask,
  handler vector, scratch word, and the 36-word `pt_regs`-layout trap frame.
  See CLAUDE.md "Hardware trap frame".
- Every CSR instruction, `MRET`, `SRET`, `WFI`, and S-mode / PMP / machine-ID /
  ENVCFG / SEED / trap-delegation encodings trap as illegal instructions. The
  Linux path is M/U NOMMU and does not use S-mode.
- The core holds no timer state. It has an `mtip` interrupt-input pin
  (`rv32i_set_mtip`, driven by the C# `ClintDevice`) and an `meip` pin
  (`rv32i_set_meip`, driven by the PLIC); `mtime`/`mtimecmp` live in
  `ClintDevice`.

## ISA support summary (read header of rv32i_core.cpp for details)

| Ext | Status | Notes |
|---|---|---|
| RV32I | yes | all 40 base integer instructions |
| M | no | MUL/DIV/REM family traps; use libcalls |
| A | no | LR/SC/AMO opcodes trap as illegal |
| F | no | FLW/FSW/FMA/OP-FP opcodes trap as illegal; use software float in guest |
| Zicsr / Zifencei | no / no | all CSR instructions trap illegal |
| Priv M/U | partial | ECALL/EBREAK via the trap-frame page; MRET, WFI, all CSR ops, S-mode trap illegal |
| D | no | use `Runtime/softfloat.c` in guest |
| C | no | all instructions 32-bit |
| B / Zb*, V | no |  |
| MMU walks | no | no satp; loads/stores use bare guest-phys |
| Misalign traps | no | host does the access; x86 handles misalign natively |

## Memory map (high level — see MEMORY_MAP.md for register-level)

| Address | Device | Guarded? |
|---|---|---|
| 0x00000000 | RAM (16 MB default) | plain |
| 0x02000000 | CLINT timer | guarded |
| 0x10000000 | UART 16550 | guarded |
| 0x10001000 | Keyboard FIFO | guarded |
| 0x10002000 | Mouse | guarded |
| 0x10003000 | Real-Time Clock | guarded |
| 0x0F000000 | Trap-frame page (IE flags, vector, 36-word frame) | plain |
| 0x20000000 | Framebuffer 320×200 RGBA | plain |
| 0x20100000 | Display control | guarded |
| 0x30000000 | Audio PCM buffer (1 MB) | plain |
| 0x30100000 | Audio control | guarded |
| 0x40000000 | Host-exit (write exit code → halt) | guarded |
| 0x80000000 | RAM for Linux example (relocatable base) | plain |

The Linux example uses a different memory layout (CLINT at 0x11000000,
SYSCON at 0x11100000, RAM base 0x80000000) to match the mini-rv32ima DTB.

## Peripheral interface

Every device implements `Core/IPeripheral.cs`:

```csharp
uint BaseAddress { get; }
uint Size { get; }
bool IsGuarded { get; }      // true → PAGE_NOACCESS + VEH dispatch
uint Read(uint offset, int width);   // width = 1, 2, or 4
void Write(uint offset, int width, uint value);
unsafe void Bind(byte* slice) {}     // only for plain (non-guarded) devices
```

If `IsGuarded` is true, register the peripheral with `MmioDispatcher` and let
the VEH handle every access. If false, `Bind` is called with the committed
host pointer and the device writes/reads memory directly (use this only for
hot bulk-data peripherals like the framebuffer and PCM buffer).

## Examples

| Project | What it does |
|---|---|
| `Examples/Runner` | Loads a bare-metal RV32I ELF and runs it. Useful baseline. |
| `Examples/Doom` | Full Doom port. Memory layout: code at 0x1000, stack at 0x9FFF00 (grows down), WAD at 0xA00000, WAD size at 0x9FFFFC. |
| `Examples/Voxel` | Voxel terrain renderer. |
| `Examples/Mp4Player` | Software video decode → framebuffer. |
| `Examples/Video` | Plays a pre-decoded sequence to the framebuffer. |
| `Examples/Sound` | Audio synthesis to the PCM buffer. |
| `Examples/Midi` | MIDI playback via the MIDI peripheral. |
| `Examples/Input` | Keyboard + mouse echo. |
| `Examples/TinyCC` | JIT compiles C inside the emulator at runtime. ~390 KB ELF containing TinyCC. Emits RV32I machine code and runs it. |
| `Examples/Linux` | Boots the RV32I Linux image produced by `Examples/Linux.Build_RV32i`. `--download` fetches the legacy mini-rv32ima serial-only kernel. |

`Examples/Linux.Build_RV32i` ports the kernel to the CPU's trap-frame ABI.
Userspace enters the kernel through an ordinary `ecall` — an earlier
`0xFFFF0000` syscall-gateway experiment was dropped, so userspace now uses the
architectural instruction and the CPU raises the usual environment-call trap.
The kernel *returns* from a trap by putting its private trap-frame pointer in
`a0` and jumping to the resume gateway at `0xFFFF0004`; the base ISA has no
`MRET`. The native CPU has no CSRs at all — all trap state lives in the
`0x0F000000` trap-frame page (see CLAUDE.md "Hardware trap frame"). The
Build_RV32i kernel codegen is rewritten to that ABI and boots to userspace; a
cached `Image-net` built against an older trap-page address must be rebuilt.
Note `PlicDevice` uses a 4 MB window (not the architectural 64 MB) so the PLIC
region does not swallow the trap-frame page at `0x0F000000`.

Bare-metal examples link against `RiscVEmulator.Tests/Runtime/` (libc, malloc,
softfloat for D-ext emulation, syscalls.c, vfs.c). Syscalls.c is misleadingly
named — most operations route through MMIO writes, not ECALL.

## Tests (RiscVEmulator.Tests)

- `EmulatorTestBase` provides `CompileC(srcFiles, elfFile)` (clang
  `--target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -nostdlib -O3 -fuse-ld=lld`),
  `RunElf` returning `(Output, ExitCode, Halted)`, and
  `RunElfFull` returning full peripheral access for post-run assertions.
- Test programs live in `RiscVEmulator.Tests/Programs/`. Pattern: write a C
  program → compile with the runtime → run → assert on stdout and exit code.
- Linker script: `RiscVEmulator.Tests/Programs/linker.ld`.

## Common pitfalls for new agents

1. **Do not add CRT dependencies to `rv32i_core.cpp`.** The DLL builds
   `-nodefaultlib`. Anything that emits a libc call (memcpy on aggregate
   copy, sqrtf libcall at -O0, std::sqrt, etc.) will fail to link in Debug.
   Provide a TU-local inline if needed (see existing `memset`, `memcpy`).

2. **Do not assume the CPU dispatches MMIO.** The dispatch lives in
   `MmioDispatcher.cs` via Windows VEH. If you're seeing reads/writes
   "vanish," check: is the peripheral registered? Did `EnsureInstalled`
   run? Is the address inside a committed `PAGE_NOACCESS` range?

3. **Endianness:** RISC-V is little-endian, x86-64 is little-endian, so
   `*(uint32_t*)(mem + addr)` just works. No swaps anywhere.

4. **EBREAK / ECALL now trap.** Privileged mode is always on. Bare-metal
   programs must exit via MMIO `0x40000000`, not EBREAK. (Existing programs
   already do this.)

5. **The C-ABI trampolines use the global `cpu`**, but `do_step` and helpers
   take `CPU_State&`. Both forms are correct — the global is there only so
   the trampoline names like `rv32i_get_pc()` can be parameter-less.

6. **Legacy `Emulator` configuration shims are gone.** There is no
   `EnablePrivMode`, `EnableMExtension`, `EnableAExtension`, or `RamOffset`
   property; the CPU handles privilege and removed extensions unconditionally.

## Networking

Networking works. `Examples/Linux` runs the libslirp-NAT'd kernel built by
`Examples/Linux.Build_RV32i`; standard sockets, DHCP and `wget` all function
inside the guest. How it fits together:

1. **Kernel** — `Examples/Linux.Build_RV32i` builds with `CONFIG_NET`,
   `CONFIG_INET`, `CONFIG_VIRTIO_MMIO` and `CONFIG_VIRTIO_NET` on. (The legacy
   `--download` mini-rv32ima image is still `CONFIG_NET=n` — serial only.)
2. **PLIC** — `Core/Peripherals/PlicDevice.cs` at `0x0C000000`. It exposes
   only a 4 MB window, not the architectural 64 MB, so it does not swallow the
   trap-frame page at `0x0F000000`.
3. **virtio-net** — `Core/Peripherals/VirtioNetDevice.cs` implements the
   virtio-mmio register layout + RX/TX virtqueues; the rings live in guest RAM
   (plain `*(mem + addr)` accesses — no special handling).
4. **IRQ pin** — `rv32i_set_meip(int level)` latches the MEIP interrupt pin;
   `check_interrupts()` consults it, so the PLIC drives the guest IRQ through
   that one exported call.
5. **Host backend** — `Core/Networking/` (`INetBackend`, `Win32NatBackend`,
   `SlirpBridgeBackend`) NATs guest traffic via `slirp_bridge.dll` +
   `libslirp-0.dll` over Win32 host-loopback; the guest gets `10.0.2.0/24` and
   auto-DHCPs eth0 at boot.
6. **DTB** — `Examples/Linux` patches in the `plic` and `virtio_mmio` nodes.

See CLAUDE.md "Guest-side networking + package install" for the `rvpkg`
package-feed flow built on top of this.

## File map for orientation

```
Native/rv32i_core.cpp    THE CPU. ~355 lines. Read the header comment first.
Native/rv32i_core.def    Export list — keep in sync with extern "C" functions.
Core/Emulator.cs         P/Invoke shell, peripheral wiring, run loop.
Core/HostMemoryReservation.cs   4 GB VA reservation + plain/guarded commits.
Core/MmioDispatcher.cs   VEH that turns AVs into IPeripheral.Read/Write.
Core/MemoryBus.cs        Peripheral registration and routing.
Core/Memory.cs           Plain RAM wrapper.
Core/ElfLoader.cs        PT_LOAD segments → entry point.
Core/Peripherals/*.cs    One file per device. Read these to learn the MMIO
                         protocols — much faster than reading MEMORY_MAP.md.
Frontend/SdlWindow.cs    Main UI loop. CPU runs on a worker thread in batches.
```

## What to do when stuck

1. Grep first. The code is small and self-documenting.
2. Read the header comment of `rv32i_core.cpp` for ISA details.
3. Read the `IPeripheral` implementations for MMIO protocols.
4. `README.md` has a step-by-step build guide and example invocations.
5. `Architecture.md` is a full RV32I instruction reference if you need to
   recall what `JALR` or `SLTIU` does.
