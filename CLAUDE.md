# CLAUDE.md — RV32I RISC-V Emulator

> Companion to `AGENTS.md` (LLM onboarding), `README.md` (user-facing guide),
> `Architecture.md` (full RV32I ISA reference), and `MEMORY_MAP.md`
> (register-level peripheral details).

## What Is This?

A RISC-V RV32I + M + A + F emulator with a native C++ CPU hot path and
C# peripherals/frontend (.NET 10). Single-hart, no D-ext (use `softfloat.c`
in the guest), no C-ext. Runs bare-metal ELF binaries on a small set of
memory-mapped peripherals (framebuffer, keyboard, mouse, audio, UART,
timer, RTC) and a real mini-rv32ima Linux kernel (`Examples/Linux`).
Flagship demo is a full Doom port.

## Solution Structure

```
Core/                    Emulator engine: P/Invoke shell, memory bus, MMIO
                         dispatcher, host VA reservation, ELF loader.
Core/Peripherals/        12 IPeripheral implementations.
Native/                  C++ CPU hot path (rv32i_core.cpp, ~550 lines, ClangCL).
Frontend/                SDL2 window (rendering, input, audio) via Silk.NET.SDL.
Examples/                Demo apps: Doom, Voxel, Mp4Player, Video, Sound,
                         Input, Midi, Runner, TinyCC, Linux.
RiscVEmulator.Tests/     Integration tests (compile C → ELF → run → assert).
  Programs/              C test programs + linker.ld.
  Runtime/               Bare-metal C runtime (libc, malloc, softfloat,
                         syscalls, vfs).
```

## Build & Test

```powershell
# Builds C++ (ClangCL) + all C# projects.
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln -p:Platform=x64

# Just the native DLL.
& "...MSBuild.exe" Native\rv32i_core.vcxproj -p:Platform=x64 -p:Configuration=Release

# Tests (requires clang + lld for riscv32 in PATH).
dotnet test --no-build RiscVEmulator.Tests
```

The native `rv32i_core.vcxproj` uses the **ClangCL** toolset (VS C++ workload
with Clang) and links `IgnoreAllDefaultLibraries=true`. The .cpp provides its
own `memset`, `memcpy`, `_fltused`, and an SSE-based `fsqrt` — do not introduce
headers that bring in CRT (`<cmath>`, `<cstring>`, `<cstdio>` etc.).

Tests compile C programs to RV32I ELF using
`clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -nostdlib -O3 -fuse-ld=lld`,
load them into the emulator, and assert on console output and exit codes.

## Architecture at a Glance

```
Emulator (C# P/Invoke shell)
  │  HostMemoryReservation → 4 GB VirtualAlloc(MEM_RESERVE | PAGE_NOACCESS)
  │  passes Base ptr to native; both sides read/write the same VA range
  │
  ├── rv32i_core.dll (C++ hot path)
  │     entire CPU state in one struct (CPU_State); single global instance
  │     do_step(CPU_State&) and helpers all take cpu by reference
  │     every load/store is *(volatile T*)(cpu.mem + addr) — no MMIO branches
  │
  ├── MemoryBus + Peripherals
  │     each peripheral commits its guest slice as PAGE_READWRITE (plain) or
  │     PAGE_NOACCESS (guarded) inside the host VA reservation
  │
  ├── MmioDispatcher (Windows VEH)
  │     catches AVs on guarded pages, decodes the faulting x86-64 MOV,
  │     dispatches to IPeripheral.Read/Write, advances RIP, resumes
  │
  └── ElfLoader  PT_LOAD segments → entry point
```

**Native hot path**: `rv32i_core.cpp` is a single-file static CPU — no classes,
all `static` functions, `static constexpr` immediate decoders,
`__builtin_expect` branch hints. The whole CPU state is a single `CPU_State`
struct; `do_step` and all helpers take it by `CPU_State&` reference.

**Host integration**: `HostMemoryReservation` reserves the entire 4 GB
guest VA up front with `VirtualAlloc(MEM_RESERVE | PAGE_NOACCESS)` — costs
only a VAD entry, no commit. Each peripheral commits its slice with
`PAGE_READWRITE` (plain memory like RAM, framebuffer, audio PCM) or
`PAGE_NOACCESS` (guarded MMIO). The C++ CPU dereferences `cpu.mem + addr`
directly. Plain pages just work; guarded pages raise AVs.

**MMIO dispatch**: `MmioDispatcher` is a process-wide vectored exception
handler installed via `AddVectoredExceptionHandler`. On AV, it looks up
which peripheral owns the page, decodes the x86 MOV at the faulting RIP
(width, GPR, sign-extend behavior), calls `IPeripheral.Read`/`Write`,
writes the result into the saved register context, and advances RIP past
the instruction. The CPU never knows anything happened.

**No ECALL callback**: ECALL/EBREAK trap to mtvec (priv mode is always on).
The bare-metal runtime exits via MMIO write to `0x40000000` (`HostExitDevice`)
and writes UART bytes via MMIO at `0x10000000`. There is no syscall hook in
the CPU.

## ISA Support

See the header of `Native/rv32i_core.cpp` for the authoritative list.

| Ext              | Status   | Notes                                                 |
|------------------|----------|-------------------------------------------------------|
| RV32I            | full     | all 40 base instructions                              |
| M                | full     | MUL/MULH[SU\|U]/DIV[U]/REM[U] — always on             |
| A                | full     | LR.W/SC.W + AMO{SWAP,ADD,XOR,AND,OR,MIN[U],MAX[U]}.W  |
| F                | full     | single-precision; no flags, RNE only, rounding ignored |
| Zicsr / Zifencei | yes / NOP| FENCE/FENCE.I are NOPs                                |
| Priv M/S/U       | full     | trap delegation (medeleg/mideleg), MRET/SRET, WFI     |
| D                | no       | provide via `Runtime/softfloat.c` in guest            |
| C                | no       | all instructions 32-bit                               |
| B / Zb*, V       | no       |                                                       |
| MMU walks        | no       | `satp` is stored but loads/stores use bare guest-phys |
| Misalign traps   | no       | host does the access; x86 handles misalign natively   |

Privileged mode is **always on**. The CPU boots in M-mode with all CSRs zero.
There is no `EnablePrivMode` toggle on the native side; the C# property of
that name on `Emulator` is an inert compat shim.

Spec-strict CSR masks (sstatus/sie/sip view masks, mip MTIP write-protect,
mepc/sepc low-bit clear) are intentionally **dropped** for code lean-ness.
Linux follows protocol and is unaffected. A third-party guest that pokes
those bits directly may misbehave — restore the masks in `priv_csr` if you
need strict compliance.

## Interrupts

`check_interrupts()` runs every step. Currently MTIP is the only externally-
visible interrupt source, auto-raised when `mtime ≥ mtimecmp`. The priority
table is wired for MEIP/MSIP/MTIP/SEIP/SSIP/STIP — all bits are settable via
CSR write from the guest, but the host has no injector for MEIP/SEIP yet
(see `AGENTS.md` "Adding networking" for the missing trampoline).

## Halting and exit code

Bare-metal programs halt by writing the exit code to MMIO `0x40000000`
(`HostExitDevice`). The peripheral's `OnExit` callback in `Emulator` sets the
C# `_cachedExitCode` field and calls `rv32i_set_halted(1)`. The native side
has no `exit_code` field.

The Linux example halts via the SYSCON peripheral at `0x11100000` (reboot or
poweroff) which sets the emulator halted flag.

## Memory Map (key regions)

| Address      | Device                                    | Guarded? |
|--------------|-------------------------------------------|----------|
| `0x00000000` | RAM (16 MB default)                       | plain    |
| `0x02000000` | CLINT timer (mtime/mtimecmp)              | guarded  |
| `0x10000000` | UART 16550 (console I/O)                  | guarded  |
| `0x10001000` | Keyboard (scancode FIFO)                  | guarded  |
| `0x10002000` | Mouse (relative deltas)                   | guarded  |
| `0x10003000` | Real-Time Clock (wall-clock µs/ms/epoch)  | guarded  |
| `0x20000000` | Framebuffer (320×200 RGBA8888)            | plain    |
| `0x20100000` | Display Control (resolution, vsync, …)    | guarded  |
| `0x30000000` | Audio PCM buffer (1 MB)                   | plain    |
| `0x30100000` | Audio Control (sample rate, channels, …)  | guarded  |
| `0x40000000` | Host-exit (write exit code → halt)        | guarded  |

`Examples/Linux` uses a different layout to match the mini-rv32ima DTB:
RAM at `0x80000000`, CLINT at `0x11000000`, SYSCON at `0x11100000`.

See `MEMORY_MAP.md` for full register-level details.

## Peripheral Interface

All peripherals implement `Core/IPeripheral.cs`:

```csharp
uint BaseAddress { get; }
uint Size        { get; }
bool IsGuarded   { get; }     // true → PAGE_NOACCESS + VEH dispatch
uint Read(uint offset, int width);             // width = 1, 2, or 4 bytes
void Write(uint offset, int width, uint value);
unsafe void Bind(byte* hostSlice) {}           // only for plain (non-guarded) devices
```

If `IsGuarded` is true, `Emulator` registers the peripheral with
`MmioDispatcher` and the VEH handles every access. If false, `Bind` is called
with the committed host pointer and the device reads/writes the bytes
directly — used for hot bulk-data peripherals (framebuffer, PCM buffer).

## Frontend (SDL2)

`SdlWindow` runs the CPU on a background thread (500k steps per batch) and
renders the framebuffer at ~120 fps on the main thread. Input events are
routed to the keyboard/mouse peripherals. Audio is drained from the PCM
buffer to SDL's audio queue. CPU and renderer are decoupled — no
synchronization; framebuffer is always live-read.

## Test Pattern

Tests inherit `EmulatorTestBase` which provides:

- `CompileC(srcFiles, elfFile)` — compile C to RV32I ELF
- `EnsureRuntimeObject()`, `EnsureLibcObject()`, etc. — cached .o files
- `RunElf(elfFile)` → `(Output, ExitCode, Halted)`
- `RunElfFull(elfFile)` → full `RunResult` with post-run access to all
  peripherals (framebuffer pixels, audio samples, RTC, etc.)
- `RunProgramWithLibc(baseName)` — compile + link with full runtime, run,
  assert exit 0

Typical test: write a C program in `Programs/`, compile with the runtime,
run, assert on stdout and exit code (or on framebuffer contents for graphics
tests).

## Key Design Decisions

- **Native C++ hot path** — CPU loop runs entirely in clang-compiled C++;
  C# is just the P/Invoke shell + peripherals.
- **Shared 4 GB VA reservation** — `HostMemoryReservation` reserves the
  guest's entire 32-bit address space on the host. Peripherals commit
  slices as plain or guarded. The C++ CPU dereferences `mem + addr`
  directly; no copies, no translation, no per-access dispatch logic.
- **MMIO via Windows VEH** — guarded pages raise AVs that a vectored
  exception handler turns into `IPeripheral.Read`/`Write` calls. The CPU
  has zero MMIO awareness.
- **`do_step(CPU_State&)`** — entire CPU state lives in one struct; the
  step function and every helper take it by reference. The global `cpu`
  exists only so parameter-less C-ABI trampolines work.
- **Priv mode always on** — no opt-in flag. M-mode at boot; bare-metal
  programs work because they exit via MMIO, never via EBREAK/ECALL.
- **No condition flags** — RV32I comparisons write results to registers
  (SLT/SLTU/branches).
- **x0 is a sink** — writes silently zeroed at end of each step.
- **F-ext is hardware** in the emulator; D-ext is software via
  `Runtime/softfloat.c` in the guest.
- **CPU–render decoupled** — no synchronization; framebuffer is always
  live-read.
- **Doom memory layout**: code at 0x1000, stack at 0x9FFF00 (grows down),
  WAD data at 0xA00000, WAD size at 0x9FFFFC.

## TinyCC Example

`Examples/TinyCC/` embeds TinyCC (mob branch, ONE\_SOURCE amalgamation) and
JIT-compiles C at runtime inside the emulator — a compiler running inside a
CPU emulator generating new machine code for that same CPU.

### What it does

The emulator runs a ~390 KB ELF containing a full C compiler. That compiler
JIT-compiles three functions from a string:

- `fib(n)` — recursive Fibonacci
- `count_primes(n)` — Sieve of Eratosthenes
- `mandelbrot(w, h)` — ASCII Mandelbrot (60×22) using 32-bit fixed-point ints

External symbols (`printf`, `putchar`) are wired into the JIT'd code via
`tcc_add_symbol` — no shared library needed.

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
    tinycc_demo.c  → _start: tcc_new / compile_string / relocate / get_symbol
    stubs.c        → Dead-code symbol stubs (file I/O, 128-bit float ops, ...)
  Program.cs       → C# launcher: compiles ELF, loads into emulator, runs
```

### Build notes

`tinycc_demo.c` is compiled with `-DONE_SOURCE=1 -DTCC_TARGET_RISCV32=1` and
`-O1` (TCC source is large; `-O3` is very slow). It `#includes libtcc.c`
which pulls in everything else.

`CONFIG_TCC_PREDEFS=1` in `config.h` causes TCC to embed `tccdefs.h` as a
compile-time C string array (via `tccdefs_.h`), bypassing the need to open
any files at runtime.

The Mandelbrot demo uses 32-bit fixed-point integers (scale = 256) because
the emulator's F-ext is single-precision and the demo wants to demonstrate
JIT'd integer code, not float.

### Run

```powershell
# Build solution first.
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln -p:Platform=x64

# Run the JIT demo.
dotnet run --no-build --project Examples\TinyCC -p:Platform=x64
```

## Linux Example

`Examples/Linux` boots a real RV32 nommu Linux kernel (mini-rv32ima image).
Run with `--download` first time to fetch the kernel + DTB.

```powershell
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --download
```

**Networking caveat**: the prebuilt kernel was built with `CONFIG_NET=n`, so
`socket()` returns `ENOSYS` and tools like `ping`/`wget` don't work. Adding
networking requires a kernel rebuild plus a host-side virtio-net peripheral
and a small IRQ-injection trampoline in `rv32i_core.cpp`. See `AGENTS.md`
"Adding networking" for the concrete plan.

### Guest-side networking + package install

The networked build comes from `Examples/Linux.Build_RV32i` (formerly
`Examples.Linux.Prepare`) which drives WSL+buildroot to produce
`~/.cache/riscvemu/linux/Image-net` and `rvemu-net.dtb`. It also bakes in:

- `slirp_bridge.dll` + `libslirp-0.dll` are loaded on the host, NAT'd via
  Win32 host-loopback into the guest as `10.0.2.0/24`.
- An `/etc/init.d/S41dhcp` overlay script that auto-DHCPs eth0 on boot
  (never blocks: 10 s max wait, unconditional `exit 0`).
- `/chosen/rng-seed = [64 random bytes]` injected at build time, so the
  CRNG is seeded at time 0 instead of after ~941 guest seconds.
- `rvpkg` — a pure POSIX-shell installer in `/usr/bin/rvpkg`. opkg itself
  can't run on nommu+uclibc (needs MMU+wchar) so we ship a 30-line
  `wget | ar x | tar -xzf` replacement that talks to a host-side package
  feed (`Examples.Linux.Packageserver`) over libslirp.
- busybox config fragment enabling `ar`, `tar`, `gunzip`, `wget` applets.
  Note: `awk` is built in but **broken** in our nommu/uclibc combination
  (returns "Access to negative field" even for `$1`). rvpkg avoids awk.

End-to-end flow that already works:

```
host:    dotnet run --project Examples\Linux.Packageserver
         > search nano        (browses all 2709 buildroot packages)
         > add bc
         > run                (cross-builds bc.ipk, serves on :8080)

guest:   rvpkg update         (fetches Packages index)
         rvpkg install bc     (downloads + unpacks the .ipk into /)
         echo 3*7 | bc        →  21
```

### Don't put `simple-framebuffer` in the DT

`FramebufferDevice` lives at guest physical `0x20000000` (256 KB). Adding
a well-formed `compatible = "simple-framebuffer"` DT node pointing there
**silently hangs the kernel before any printk**. Root cause:

1. The kernel sees the FB node's reg `<0x00 0x20000000 0x00 0x40000>` as a
   memory bank and `memblock_add`s it.
2. RISC-V's `phys_ram_base = memblock_start_of_DRAM()` drops to
   `0x20000000`, so `ARCH_PFN_OFFSET = 0x20000` and `max_mapnr` now spans
   the FB plus the gap up to RAM at `0x80000000`.
3. `memmap_init` calls `init_unavailable_range(0x20040, 0x80000)` to fill
   `struct page` slots for the 393k-page gap. `pfn_valid` is true for
   every PFN in that gap, so the loop walks every page individually,
   `__memset`-ing each. At ~50 MIPS this is effectively forever.

Bisect signal: `RVEMU_TRACE=1 dotnet ... Examples.Linux.dll` prints
`mtime + PC` every 2 s. The hang shows the PC pinned to `__memset` /
`init_unavailable_range` per the `output/build/linux-*/System.map` of the
prepared kernel. Without proper `#address-cells = 2; #size-cells = 2;` on
the parent of the FB node, the reg parses as size=0 and the kernel
silently discards the node → "minimal node booted" is a false positive.

**Workarounds**: we've adopted **#3** (userspace `mmap /dev/mem`). The FB
now lives at `0x85FC0000` (last 256 KB of a 96 MB RAM bank), no
`simple-framebuffer` DT node is emitted, and `CONFIG_DEVMEM=y` lets guest
userspace mmap it directly. `Examples.Linux.Build_RV32i/guest-userspace/
rvemu-fbtest.c` is the reference demo: opens `/dev/mem`, mmaps the FB,
draws to it, reads `/dev/input/event0+1` for input. Pass `--gui` to
`Examples.Linux` to see it rendered in an SDL window.

The other two paths (FB inside RAM with `/reserved-memory` + `/chosen/
framebuffer`; custom `"rvemu,framebuffer"` driver) were tried — the first
re-triggers `init_unavailable_range` even with proper memblock layout, and
the second is more code than option 3 plus needs a kernel patch.

Bonus trap: when stripping cells decls via regex, anchor to the right
subtree. The root `/` and `/chosen` both write `#address-cells = <0x02>;`
identically, so a naive `re.sub` first-match strips the root's, which
makes the kernel ignore `/memory@80000000` and panic with
`early_init_dt_alloc_memory_arch: Failed to allocate N bytes`.

### Linux input + framebuffer (working stack)

`Examples.Linux` defaults to 96 MB RAM. The last 256 KB are reserved for
the framebuffer at `0x85FC0000` (registered by `FramebufferDevice`).
`KeyboardDevice` and `MouseDevice` are registered at `0x10001000` /
`0x10002000` (same MMIO addresses as Doom).

Inside the guest, `rvemu-input` is a tiny BFLT daemon (built by
`Examples.Linux.Build_RV32i`, source at `guest-userspace/rvemu-input.c`)
that `mmap`s those MMIO regions via `/dev/mem`, polls them, and synthesizes
`input_event`s into `/dev/uinput`. `/etc/init.d/S42input` auto-starts it at
boot. The result: `/dev/input/event0` (kbd) and `/dev/input/event1` (mouse)
behave like normal Linux evdev devices for any app.

Guest userspace apps display by `mmap`ing `/dev/mem` at `0x85FC0000` and
writing RGBA pixels. `rvemu-fbtest` is the reference (color gradient + a
cursor that follows the host mouse). On the host, `--gui` opens an SDL
window that shows `FramebufferDevice.PresentedPixels` and forwards
keyboard/mouse events into the C# `KeyboardDevice` / `MouseDevice`
peripherals — closing the loop with the guest's `rvemu-input` daemon.

**BFLT cross-compile recipe** (post-mortem in
`memory/project_nommu_bflt_quirks.md`): the magic incantation is
```
$CC -static -fPIC -Wl,-elf2flt=-r -O2 src.c -o out
```
Without `-Wl,-elf2flt=-r` the binary loads as a process but never reaches
`main()`. `Examples.Linux.Build_RV32i/Program.cs` drives the cross-compile
via a small shell script and drops the results into the rootfs overlay.

### Microwindows nano-X (real retained-mode WM)

`Examples.Linux.Build_RV32i` optionally rolls in a Microwindows nano-X
server + window manager + demo clients. The patched upstream tree lives
at `$HOME/rvemu-mw/microwindows` (cloned from
`https://github.com/ghaerr/microwindows`); the rvemu-specific overrides
are kept beside `Examples/Linux.Build_RV32i/microwindows/`
(`config`, `scr_rvemu.c`) and a one-shot setup script lives at
`Examples/Linux.Build_RV32i/scripts/build-microwindows.sh`.

Five things needed patching:

1. New `UCLINUX-RISCV` arch in `src/Arch.rules` adding
   `-Wl,-elf2flt=-r` so each binary becomes BFLT v4 ram gotpic.
2. New screen driver `src/drivers/scr_rvemu.c` that opens `/dev/mem`
   and `mmap`s the FB at `0x85FC0000` directly (we have no `/dev/fb0`,
   see above). Also defines stub `ioctl_get/setpalette` because
   `kbd_ttyscan.c` references them unconditionally.
3. `SCREEN=RVEMU` selector added to `src/drivers/Objects.rules`.
4. `AUTO_START_SERVER=0` in `src/include/mwconfig.h` — default uses
   `fork()` from any client to spawn the server; nommu has no fork.
5. Strip `nxlaunch`, `nxterm`, `nxroach`, `nxev` (all fork-using) and
   the C++ `cannyedgedetect` / `demo-agg` demos (toolchain has no g++)
   from `src/demos/nanox/Makefile`. Enable the previously-commented
   `nanowm` target while you're there.

When the prepare detects pre-built binaries under
`$HOME/rvemu-mw/microwindows/src/bin/`, it copies `nano-X`, `nanowm`,
and a few demo clients into the rootfs overlay and installs
`S45microwindows` which:

1. Stops the makeshift `rvemu-desktop` (S43desktop) if running.
2. Starts `nano-X` (waits for `/tmp/.nano-X` socket).
3. Starts `nanowm` (window manager).
4. Starts `nxclock` + `nxeyes` as initial visible apps.

To launch additional demos by hand: just `nxchess &` / `demo-arc &` etc.
at the shell — they connect to the running server over the socket.
