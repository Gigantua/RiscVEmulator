# CLAUDE.md — RV32I RISC-V Emulator

## What Is This?

A RISC-V RV32I emulator with a native C++ CPU hot path and C# peripherals/frontend (.NET 10). It runs bare-metal ELF binaries with memory-mapped peripherals (framebuffer, keyboard, mouse, audio, UART, timer, RTC). The flagship demo is a full Doom port.

## Solution Structure

```
Core/                    → Emulator engine (P/Invoke shell, memory bus, peripherals)
Native/                  → C++ CPU hot path (rv32i_core.cpp, ClangCL vcxproj)
Frontend/                → SDL2 window (rendering, input, audio) via Silk.NET.SDL
Examples/                → Demo apps: Doom, Video, Sound, Input, Runner, TinyCC
RiscVEmulator.Tests/     → Integration tests (compile C → ELF → run → assert)
  Programs/              → C test programs + linker.ld
  Runtime/               → Bare-metal C runtime (libc, malloc, softfloat, syscalls, vfs)
```

## Build & Test

```powershell
msbuild RiscVEmulator.sln                # builds C++ (ClangCL) + all C# projects
dotnet test --no-build RiscVEmulator.Tests  # requires clang + lld for riscv32 in PATH
```

The native `rv32i_core.vcxproj` uses the **ClangCL** toolset (requires VS C++ workload with Clang). It builds `rv32i_core.dll` which propagates to all C# output dirs via ProjectReference.

Tests compile C programs to RV32I ELF using `clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -nostdlib -O3 -fuse-ld=lld`, load them into the emulator, and assert on console output and exit codes.

## Architecture at a Glance

```
Emulator (C# P/Invoke shell)
  │  pins Memory.Data[] → passes IntPtr to native
  │
  ├── rv32i_core.dll (C++ hot path)
  │     CPU registers, PC, mtime — all live in native
  │     step loop runs entirely in C++
  │     MMIO/ECALL → callbacks into C#
  │
  ├── MemoryBus           Routes MMIO by address → peripheral
  │     ├── Memory        Byte-array RAM (pinned, shared with C++)
  │     └── IPeripheral[] MMIO devices (UART, timer, framebuffer, etc.)
  └── ElfLoader           Loads PT_LOAD segments, returns entry point
```

**Native hot path**: `rv32i_core.cpp` is a single-file static CPU — no classes, all static functions, `constexpr` decode helpers, `__builtin_expect` branch hints, `__builtin_memcpy` for aligned memory ops. RAM is a pinned C# byte[] shared via IntPtr (zero-copy).

**MMIO**: Addresses ≥ RAM size trigger a callback into C# which routes through MemoryBus to peripherals. CLINT mtime reads are intercepted — native owns the counter.

**ECALL**: Callback into C# handles exit (a7=93/94) and write (a7=64) syscalls. Write reads directly from the pinned RAM array.

## ISA Support

- **RV32I** — all 40 base instructions
- **M-extension** — MUL/MULH/DIV/REM family, opt-in via `Emulator.EnableMExtension = true`. Doom enables it by default (`--no-m-ext` to disable). Runner accepts `--m-ext`.
- No F/D hardware — software float/double in `Runtime/softfloat.c`
- No interrupts/exceptions — EBREAK halts, FENCE is NOP

## Syscalls (ECALL, a7 = syscall number)

| a7 | Name | Behavior |
|----|------|----------|
| 64 | write | Output bytes from memory to `OutputHandler` |
| 93 | exit | Halt, set `ExitCode = a0` |
| 94 | exit_group | Same as exit |

Other syscall numbers are silently ignored. The C runtime in `Runtime/syscalls.c` wraps more (time, file I/O via VFS).

## Memory Map (key regions)

| Address | Device |
|---------|--------|
| `0x00000000` | RAM (16 MB) |
| `0x02000000` | CLINT Timer (mtime counts CPU steps, not wall-clock) |
| `0x10000000` | UART 16550 (console I/O) |
| `0x10001000` | Keyboard (scancode FIFO) |
| `0x10002000` | Mouse (relative deltas) |
| `0x10003000` | Real-Time Clock (wall-clock µs/ms/epoch) |
| `0x20000000` | Framebuffer (320×200 RGBA8888) |
| `0x20100000` | Display Control (resolution, vsync, palette) |
| `0x30000000` | Audio PCM Buffer (1 MB) |
| `0x30100000` | Audio Control (sample rate, channels, play/stop) |

See `MEMORY_MAP.md` for full register-level details.

## Peripheral Interface

All peripherals implement `IPeripheral`:
```csharp
uint BaseAddress { get; }
uint Size { get; }
uint Read(uint offset, int width);   // width = 1, 2, or 4 bytes
void Write(uint offset, int width, uint value);
```

## Frontend (SDL2)

`SdlWindow` runs the CPU on a background thread (500k steps per batch) and renders the framebuffer at ~120 fps on the main thread. Input events are routed to peripheral devices. Audio is drained from the PCM buffer to SDL's audio queue.

## Test Pattern

Tests inherit `EmulatorTestBase` which provides:
- `CompileC(srcFiles, elfFile)` — compile C to RV32I ELF
- `EnsureRuntimeObject()`, `EnsureLibcObject()`, etc. — cached .o files for the runtime
- `RunElf(elfFile)` → `(Output, ExitCode, Halted)`
- `RunElfFull(elfFile)` → full `RunResult` with access to all peripherals post-run
- `RunProgramWithLibc(baseName)` — compile + link with full runtime + run + assert exit 0

A typical test: write a C program in `Programs/`, compile with runtime, run, assert on printed output.

## Key Design Decisions

- **Native C++ hot path** — CPU loop runs entirely in clang-compiled C++; C# is just the P/Invoke shell + peripherals
- **Shared RAM via pinning** — C# allocates RAM, pins it with GCHandle, passes IntPtr to native. Both sides read/write the same memory. Zero copy.
- **No condition flags** — RV32I comparisons write results to registers (SLT/SLTU/branches)
- **x0 is a sink** — writes silently discarded
- **Software MUL/DIV and float** — runtime.c and softfloat.c emulate M/F/D extensions in C
- **CPU–render decoupled** — no synchronization; framebuffer is always live-read
- **Doom memory layout**: code at 0x1000, stack at 0x9FFF00 (grows down), WAD data at 0xA00000, WAD size at 0x9FFFFC

## TinyCC Example

`Examples/TinyCC/` embeds TinyCC (mob branch, ONE\_SOURCE amalgamation) and JIT-compiles C at runtime inside the emulator — a compiler running inside a CPU emulator generating new machine code for that same CPU.

### What it does

At runtime, the emulator runs a ~390 KB ELF that contains a full C compiler. That compiler JIT-compiles three functions from a string:
- `fib(n)` — recursive Fibonacci
- `count_primes(n)` — Sieve of Eratosthenes
- `mandelbrot(w, h)` — ASCII Mandelbrot set (60×22) using 32-bit fixed-point integers

External symbols (`printf`, `putchar`) are wired into the JIT'd code via `tcc_add_symbol` — no shared library needed.

### Structure

```
Examples/TinyCC/
  tinycc/          → TinyCC mob branch sources (ONE_SOURCE pattern)
    libtcc.c       → Top-level; #includes all others when ONE_SOURCE=1
    tcc.h          → Modified: RISCV32 target, pcrel_hi guards
    config.h       → Bare-metal config: no paths, CONFIG_TCC_PREDEFS=1
    riscv32-gen.c  → RV32I code generator (adapted from riscv64-gen.c)
    riscv32-link.c → RV32I ELF relocator (R_RISCV_32, ELF_START_ADDR=0x1000)
    tccdefs_.h     → tccdefs.h converted to C strings (avoids runtime file I/O)
    tccgen.c, tccpp.c, tccelf.c, tccrun.c, tccasm.c, tccdbg.c
  include/         → Bare-metal stub headers (stdint.h, signal.h, setjmp.h, ...)
  Programs/
    tinycc_demo.c  → _start: calls tcc_new/compile_string/relocate/get_symbol
    stubs.c        → Dead-code symbol stubs (file I/O, 128-bit float ops, ...)
  Program.cs       → C# launcher: compiles ELF, loads into emulator, runs
```

### Build notes

`tinycc_demo.c` is compiled with `-DONE_SOURCE=1 -DTCC_TARGET_RISCV32=1` and `-O1` (TCC source is large; `-O3` is very slow). It `#includes libtcc.c` which pulls in everything else.

`CONFIG_TCC_PREDEFS=1` in `config.h` causes TCC to embed `tccdefs.h` as a compile-time C string array (via `tccdefs_.h`), bypassing the need to open any files at runtime.

The Mandelbrot demo uses 32-bit fixed-point integers (scale = 256) rather than floating-point because the emulator runs RV32I+M only — no F/D hardware float extension.

### Run

```powershell
# Build solution first (requires msbuild with ClangCL):
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln -p:Platform=x64

# Run the TinyCC JIT demo:
dotnet run --no-build --project Examples\TinyCC -p:Platform=x64
```
