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
Native/          C++ CPU hot path. rv32i_core.cpp (~550 lines), ClangCL vcxproj.
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

- All CPU state lives in one struct: `CPU_State` (regs, pc, mtime, mtimecmp,
  mem pointer, priv_mode, wfi_pending, CSRs).
- `do_step(CPU_State& cpu)` and every helper it calls take `cpu` by reference.
  A single global `static CPU_State cpu;` exists for the C-ABI trampolines
  (`rv32i_step_n`, `rv32i_init`, `rv32i_get_pc`, etc.) — they pass `cpu` into
  the parameterized inner functions.
- Privileged mode is **always on**. The CPU boots in M-mode (`priv_mode = 3`,
  all CSRs zero). Bare-metal programs that don't ECALL/EBREAK/touch CSRs are
  unaffected; they exit via MMIO write to `0x40000000` (`HostExitDevice`).
- `priv_csr(cpu, csrno)` returns a `uint32_t&` to the CSR's backing slot.
  Reads dereference it; writes assign through it. RO and computed CSRs
  (misa, mvendorid, mtime aliases) stage into a static scratch — writes
  there are silently discarded, which is exactly RO semantics.
- Spec-strict CSR masks (sstatus/sie/sip view masks, mip MTIP write-protect,
  mepc/sepc low-bit clear) are intentionally dropped for code lean-ness.
  Linux follows protocol so doesn't trip them; restore if a third-party
  guest needs strict compliance.
- mtime is incremented once per `do_step`. mtime ≥ mtimecmp auto-raises
  MTIP each step. Other interrupt bits (MEIP/SEIP, MSIP/SSIP) are writable
  via CSR but have no host-side injector yet — that's the missing piece for
  virtio-net (see "Adding networking" below).

## ISA support summary (read header of rv32i_core.cpp for details)

| Ext | Status | Notes |
|---|---|---|
| RV32I | full | all 40 base instructions |
| M | no | MUL/DIV/REM family traps; use libcalls |
| A | no | LR/SC/AMO opcodes trap as illegal |
| F | no | FLW/FSW/FMA/OP-FP opcodes trap as illegal; use software float in guest |
| Zicsr / Zifencei | yes / NOP | FENCE is a NOP (single-hart, no I-cache) |
| Priv M/S/U | yes | trap delegation, MRET/SRET, WFI, ECALL/EBREAK |
| D | no | use `Runtime/softfloat.c` in guest |
| C | no | all instructions 32-bit |
| B / Zb*, V | no |  |
| MMU walks | no | satp is stored but loads/stores use bare guest-phys |
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

6. **`EnablePrivMode`, `EnableMExtension`, `EnableAExtension`, `RamOffset`**
   on `Emulator` are inert compatibility
   shims — the CPU handles everything unconditionally. Don't introduce new
   call sites that rely on them doing anything.

7. **`Examples/Linux` requires `EnablePrivMode = true`** as a vestigial
   line — it's harmless but no longer functional. Removing it is a no-op.

## Adding networking (FAQ)

A common request: "make sockets work in `Examples/Linux`." Path:

1. The prebuilt kernel has `CONFIG_NET=n` → no `AF_INET`. Must rebuild
   with `CONFIG_NET=y`, `CONFIG_INET=y`, `CONFIG_VIRTIO_MMIO=y`,
   `CONFIG_VIRTIO_NET=y` (or `CONFIG_SLIP=y` + a second UART for a
   simpler-but-slower path).
2. Add a C# `PlicDevice` (PLIC MMIO peripheral) at `0x0C000000`.
3. Add a C# `VirtioNetDevice` implementing virtio-mmio register layout +
   RX/TX virtqueues. Backing data ring lives in guest RAM (plain
   `*(mem + addr)` accesses — no special handling).
4. Add **one** thing to the C++ CPU: a host-side IRQ injector
   (`rv32i_set_meip(int level)`) that flips bit 11 in `cpu.csr_mip`.
   `check_interrupts()` already walks MEIP/SEIP via the priority array,
   so once the bit is settable from outside, the trap path Just Works.
5. Patch the DTB to add `plic` and `virtio_mmio` nodes (Examples/Linux
   already patches RAM size — extend that step).

The C++ delta is ~30 lines (one trampoline). The C# delta is a few hundred.
The kernel rebuild is the long pole.

## File map for orientation

```
Native/rv32i_core.cpp    THE CPU. ~550 lines. Read the header comment first.
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
