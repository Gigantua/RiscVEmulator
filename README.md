# RISC-V Emulator

> ### The world's first Linux on a bare RV32I core — no extensions.
>
> A real Linux 6.6 kernel — preemptive multitasking, interrupts, M/U-mode
> transitions, a full network stack, a package manager and a windowed
> desktop — running on a CPU that implements **nothing but the 40 base
> RV32I instructions**. No multiply, no divide, no atomics, no float, no
> compressed encodings, *no CSRs*. Any opcode outside the base ISA is
> invalid and traps.

![Linux](https://img.shields.io/badge/boots-Linux%206.6-brightgreen)
![Doom](https://img.shields.io/badge/runs-Doom-red)
![TinyCC](https://img.shields.io/badge/compiles-C%20in%20TinyCC-blue)
![ISA](https://img.shields.io/badge/ISA-pure%20RV32I-orange)
![.NET 10](https://img.shields.io/badge/.NET-10-blueviolet)

The instruction core is **one C++ `switch` statement, about 100 lines long**.
The trap unit that turns it into a machine capable of running an operating
system adds **roughly ten more**. There is no JIT, no MMU, no microcode ROM —
and yet it boots a genuine Linux distribution to a graphical desktop.

This repository is a monorepo of three RISC-V emulators built on one design.
The headline — and what this README is about — is
[**`RiscVEmulator-RV32I`**](RiscVEmulator-RV32I/): a pure RV32I machine that
runs Linux. The other two cores trade that minimalism for a conventional
privileged architecture and a full 64-bit machine; see
[The three cores](#the-three-cores) below.

---

## What it can do

| | |
|---|---|
| 🐧 **Linux 6.6 + desktop** | A real nommu kernel boots to a **graphical Microwindows desktop** — taskbar, terminal, clock, eyes, calculator, chess, tetris. Drag windows, click buttons, type into apps. |
| 💻 **A real terminal** | `rvemu-term` runs `sh -i` over a Unix98 pty with full VT100 escape parsing — `nano`, `vi`, `less`, `top` all work. |
| 🎮 **Doom** | Full DOOM, two ways: bare-metal PureDOOM, and a windowed `doomgeneric` client running *inside* the Linux desktop. |
| 🛜 **Networking** | A complete TCP/IP stack — host-loopback NAT through libslirp. DHCP, DNS, `wget`, sockets. The guest's `eth0` is a memory-mapped device with a thin Linux driver. |
| 📦 **Package manager** | `rvpkg` installs software from a host-side feed that cross-compiles any of ~2700 buildroot packages on demand. `zork`, `sl`, `bc`, `nano` — installed and run from inside the guest. |
| 🔊 **Stereo audio, RTC, MIDI** | PCM audio, real-time clock and MIDI output peripherals, reachable from guest userspace through ordinary device files. |
| ⚙️ **TinyCC** | A C compiler running inside the emulator, compiling C for the very CPU it runs on. |
| 🌄 **Video / Voxel** | Software-rendered video playback and a height-map voxel renderer, written entirely in guest C. |

Everything above talks to the outside world through **memory-mapped I/O and
nothing else**. Thin Linux driver wrappers turn ordinary device files —
`/dev/fb0`, `/dev/snd`, `eth0`, `/dev/input/event*` — into plain loads and
stores against host peripherals.

---

## The entire CPU

Hand the core anything outside RV32I — a `SYSTEM` opcode, or an `M`/`A`/`F`
encoding — and it does not pretend to execute it. It returns the exception
and leaves `pc` on the offending instruction, for the surrounding trap unit
to deal with. That is the whole datapath:

```cpp
// Execute one base RV32I instruction. On success advances pc and returns
// {EXC_NONE}. On a SYSTEM opcode or a non-RV32I encoding it returns the
// exception and leaves pc on the offending instruction.
static CpuException cpu_step(CPU_State& cpu) {
    const uint32_t instr = mem_read<uint32_t>(cpu, cpu.pc);
    const int      rd    = (instr >>  7) & 0x1F;
    const uint32_t f3    = (instr >> 12) & 0x7;
    const uint32_t f7    = (instr >> 25) & 0x7F;
    const uint32_t u1    = cpu.regs[(instr >> 15) & 0x1F];   // rs1
    const uint32_t u2    = cpu.regs[(instr >> 20) & 0x1F];   // rs2
    const int32_t  s1    = (int32_t)u1;
    const int32_t  s2    = (int32_t)u2;
    uint32_t nextpc      = cpu.pc + 4;

    switch (instr & 0x7F) {

    case 0x37: cpu.regs[rd] = instr & 0xFFFFF000u;                        break;  // LUI
    case 0x17: cpu.regs[rd] = cpu.pc + (instr & 0xFFFFF000u);             break;  // AUIPC
    case 0x6F: cpu.regs[rd] = cpu.pc + 4; nextpc = cpu.pc + j_imm(instr); break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;               // JALR
                 cpu.regs[rd] = cpu.pc + 4; nextpc = t;                   break; }

    case 0x63: {                                                                 // BRANCH
        int taken = 0;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
        }
        if (taken) nextpc = cpu.pc + b_imm(instr);
        break;
    }

    case 0x03: {                                                                 // LOAD
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: cpu.regs[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (cpu, addr); break;
            case 1: cpu.regs[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(cpu, addr); break;
            case 2: cpu.regs[rd] =                    mem_read<uint32_t>(cpu, addr); break;
            case 4: cpu.regs[rd] =                    mem_read<uint8_t> (cpu, addr); break;
            case 5: cpu.regs[rd] =                    mem_read<uint16_t>(cpu, addr); break;
        }
        break;
    }

    case 0x23: {                                                                 // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (cpu, addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(cpu, addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(cpu, addr,           u2); break;
        }
        break;
    }

    case 0x13: {                                                                 // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        if ((f3 == 1 && f7 != 0x00) || (f3 == 5 && f7 != 0x00 && f7 != 0x20))
            return { EXC_ILLEGAL, instr };
        uint32_t r = 0;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                         break;  // ADDI
            case 1: r = u1 << sh;                                     break;  // SLLI
            case 2: r = s1 < imm           ? 1u : 0u;                 break;  // SLTI
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                 break;  // SLTIU
            case 4: r = u1 ^ (uint32_t)imm;                           break;  // XORI
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh; break;  // SRAI/SRLI
            case 6: r = u1 | (uint32_t)imm;                           break;  // ORI
            case 7: r = u1 & (uint32_t)imm;                           break;  // ANDI
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x33: {                                                                 // OP
        if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)))
            return { EXC_ILLEGAL, instr };
        const int sh = s2 & 0x1F;
        uint32_t r = 0;
        switch (f3) {
            case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;  // SUB/ADD
            case 1: r = u1 << sh;                                              break;  // SLL
            case 2: r = s1 < s2 ? 1u : 0u;                                     break;  // SLT
            case 3: r = u1 < u2 ? 1u : 0u;                                     break;  // SLTU
            case 4: r = u1 ^ u2;                                               break;  // XOR
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;          break;  // SRA/SRL
            case 6: r = u1 | u2;                                               break;  // OR
            case 7: r = u1 & u2;                                               break;  // AND
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x0F: break;                                                            // FENCE → NOP

    case 0x73: return { EXC_SYSTEM,  instr };   // SYSTEM — belongs to the environment
    default:   return { EXC_ILLEGAL, instr };   // not an RV32I encoding
    }

    cpu.regs[0] = 0;       // x0 is hardwired to zero
    cpu.pc = nextpc;
    return { EXC_NONE, 0 };
}
```

That is a pure integer datapath. It cannot multiply, it cannot do an atomic,
it has no notion of a privileged register. The interesting part is everything
it *doesn't* do — and how Linux runs anyway.

---

## How do you boot Linux on 40 instructions?

Five things stand between "a base RV32I core" and "a machine running a modern
kernel". None of them needed a new opcode.

### Gap 1 — No multiply or divide

The easy one. The toolchain is told the target has no `M` extension, so the
compiler lowers every C `*`, `/` and `%` to a **function call** —
`__mulsi3`, `__divsi3`, `__udivsi3`, … — instead of emitting an opcode. Those
helpers are pure shift-and-add routines, themselves valid RV32I. The kernel
links its own copies; userspace resolves them from libgcc.

### Gap 2 — No atomics

A single-core machine *could* treat `lr`/`sc`/`amo*` as ordinary
load-op-store — but this CPU will not even **decode** an `A` opcode, so every
atomic has to be gone before the binary ever runs. The kernel's RISC-V
atomic, bitop, xchg, cmpxchg and futex helpers are **patched** to single-hart,
no-`A` sequences; uClibc-ng's linuxthreads atomics are stripped the same way.
Sound, because there is genuinely only one hart. The patches live in
[`RiscVEmulator-RV32I/Examples/Linux.Build_RV32i/board-patches/`](RiscVEmulator-RV32I/Examples/Linux.Build_RV32i/).

### Gap 3 — No CSRs *(the interesting one)*

RV32I has no `csrr`/`csrw`. There is nowhere to keep `mtvec`, `mepc`,
`mcause`, `mscratch`, `mstatus`, `mie`. So a **fixed RAM page at
`0x0F000000`** holds what those registers would hold. Every CSR access in the
kernel's trap path is rewritten — by patch — into an ordinary `lw`/`sw`
against that page.

The save-and-restore that hardware normally hides inside a trap is, here, a
short assembly routine sitting at that page: spill `x1..x31` + `epc` +
`cause`, enter M-mode, jump to the vector. The frame it writes *is* the Linux
`struct pt_regs`, so the kernel's own trap frame is the hardware trap frame —
no translation copy. The CPU ends up running, in plain RV32I, the microcode
that a real chip would have buried inside `csrr` — and all of it is just
`lw` and `sw`.

> **"Save state, enter M-mode, vector."** Done in assembly at `0x0F000000`.
> That is the trick.

### Gap 4 — Interrupts with no interrupt instruction

One interrupt pin. Everything routes through a single trap mechanism:

| Event | Goes to |
|-------|---------|
| Timer / external interrupt hits a user process | `do_trap` |
| Timer interrupt hits the kernel | `do_trap` |
| `ecall` from userspace | `do_trap` |
| Return to a user task | `trap_return` |

Preemptive multitasking, `ecall`, the M↔U-mode transition — they are not
features that were added. They **fall out of the trap system as a side
effect**, which is exactly why the CPU stays so small.

### Gap 5 — No packages for this architecture

Nobody ships binaries for "RV32I, no extensions". So a package server runs on
the **host**: it cross-compiles any package that has source for RV32I and
serves it on port `8080`. The guest has a memory-mapped `eth0` with a working
driver, so from inside Linux `rvpkg add <name>` just works. Verified with
`zork`, the `sl` locomotive, `bc`, `nano` and others.

### Why this is interesting

The whole machine is *elegant*: preemption, `ecall`, and M-mode are emergent
properties of one trap loop, not bespoke CPU logic — a drastic cut in
complexity. **Busybox boots in about 1.5 seconds.** And because the core is
plain `constexpr`-friendly C++ with almost no state, nothing fundamentally
stops it from running massively in parallel — on a GPU, for instance, grouping
threads by next-PC to tame warp divergence.

**Limitations.** Without an MMU there is no memory protection — yet you still
get multitasking, full Linux, and `wget`. No MMU also means no dynamic library
loading: statically-linked userspace only.

---

## The three cores

The repository holds three sibling emulators, one design, a deliberate ladder
from "the most minimal RISC-V that runs Linux" up to a full 64-bit machine.

| Folder | CPU ISA | Trap model | Boots |
|--------|---------|------------|-------|
| [`RiscVEmulator-RV32I`](RiscVEmulator-RV32I/) | `RV32I` — base only | **No CSRs** — trap-frame page at `0x0F000000` | Linux 6.6 nommu + desktop |
| [`RiscVEmulator-RV32IMA_Zicsr`](RiscVEmulator-RV32IMA_Zicsr/) | `RV32I` + `Zicsr`/`Zifencei` | Architectural CSRs, M/S/U, trap delegation, `MRET`/`SRET` | Linux 6.1 nommu |
| [`RiscVEmulator-RV64GC`](RiscVEmulator-RV64GC/) | `RV64GC` (`IMAFDC`) | Full M/S/U + **Sv39 MMU** + SBI firmware | Alpine Linux `riscv64`, IceWM/XFCE |

`RV32IMA_Zicsr` keeps the conventional RISC-V privileged spec — the operative
distinction is the `Zicsr` CSRs; the integer datapath is still RV32I (`M`
lowers to libcalls). `RV64GC` is the full 64-bit machine with an MMU and a
real distribution. Each folder is a self-contained Visual Studio solution with
its own README — the rest of this document is about `RiscVEmulator-RV32I`.

---

## Repository Layout

```
RiscVEmulator-RV32I/         ← the pure-RV32I core (this README)
RiscVEmulator-RV32IMA_Zicsr/ ← same core + standard Zicsr privileged spec
RiscVEmulator-RV64GC/        ← 64-bit RV64GC + Sv39 MMU + Alpine Linux

  └─ inside RiscVEmulator-RV32I/
     Core/                   C# emulator engine (P/Invoke shell, memory bus, peripherals)
     Native/                 C++ CPU hot path — single file, ClangCL
     Frontend/               SDL2 window (rendering, input, audio) via Silk.NET.SDL
     Examples/
       Doom/                 Bare-metal Doom (PureDOOM, compiles at launch)
       Linux/                Boot the nommu kernel + nano-X desktop (--gui)
       Linux.Build_RV32i/    WSL-driven buildroot prepare: toolchain, kernel,
                             busybox, Microwindows, kernel patches → initramfs
       Linux.Packageserver/  Cross-build any buildroot package, serve as a feed
       Runner/ Video/ Voxel/ Sound/ Input/ TinyCC/   demos
     RiscVEmulator.Tests/    Integration tests (compile C → ELF → run → assert)
```

---

## Architecture

```
Emulator (C# P/Invoke shell)
  │  reserves the guest's address space on the host; both sides share the VA range
  │
  ├── rv32i_core.dll  (C++ hot path — ClangCL)
  │     entire CPU state in one CPU_State struct
  │     every guest access is one *(volatile T*)(mem + addr) dereference
  │
  ├── MemoryBus + Peripherals
  │     plain committed pages for RAM / framebuffer / audio PCM;
  │     guarded PAGE_NOACCESS pages for MMIO registers
  │
  ├── MmioDispatcher (Windows VEH)
  │     access violations on guarded pages are decoded from the faulting
  │     x86-64 MOV and dispatched to IPeripheral.Read/Write — the CPU
  │     never knows MMIO happened
  │
  └── ElfLoader   PT_LOAD segments → entry point
```

The C++ CPU runs the hot loop; C# is the peripheral shell. Memory is
zero-copy: the host reserves the guest's address space and the native core
dereferences it directly. MMIO needs no branch in the CPU — guarded pages
fault, and a vectored exception handler turns the fault into a peripheral
call.

---

## Memory Map

| Address | Size | Device |
|---------|------|--------|
| `0x00000000` | configurable | RAM |
| `0x02000000` | 64 KB | CLINT timer (`mtime`/`mtimecmp`) |
| `0x0F000000` | — | **Trap-frame page** — the stand-in for `mtvec`/`mepc`/`mcause`/… |
| `0x10000000` | 256 B | UART 16550 (console I/O) |
| `0x10001000` | 256 B | Keyboard (scancode FIFO) |
| `0x10002000` | 256 B | Mouse (relative deltas + buttons) |
| `0x10003000` | 256 B | Real-Time Clock |
| `0x20000000` | 256 KB | Framebuffer (RGBA8888) |
| `0x30000000` | 1 MB | Audio PCM buffer |
| `0x40000000` | — | Host-exit (write exit code → halt) |

The Linux example uses a different layout to match its device tree. See
[`RiscVEmulator-RV32I/MEMORY_MAP.md`](RiscVEmulator-RV32I/MEMORY_MAP.md) for
register-level detail.

---

## Build

```powershell
cd RiscVEmulator-RV32I

# Build the C++ core + every C# project (adjust the VS path to your install)
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    RiscVEmulator.sln -p:Platform=x64
```

**Prerequisites** (host is Windows-only):

| Tool | Purpose |
|------|---------|
| [.NET 10 SDK](https://dotnet.microsoft.com/download) | Build and run the C# projects |
| Visual Studio 2022 / 2026 + **C++ workload + Clang/LLVM** | Build the native core (ClangCL toolset) |
| [LLVM/Clang](https://releases.llvm.org/) + `lld` in `PATH` | Cross-compile bare-metal RV32I ELF guests |
| **WSL2 + Ubuntu** | Only for building the Linux image — buildroot runs in WSL |

---

## Examples

All commands below are run from inside `RiscVEmulator-RV32I/`.

### Linux desktop

```powershell
# First time: build the kernel + rootfs (buildroot in WSL, ~30-40 min)
dotnet run --no-build --project Examples\Linux.Build_RV32i -p:Platform=x64
# Then boot to the graphical desktop:
dotnet run --no-build --project Examples\Linux -p:Platform=x64 -- --gui
```

`--gui` opens an SDL window onto the framebuffer; without it you get the
serial console (log in as `root`, no password). The desktop is a real
Microwindows nano-X server with a window manager and a taskbar that picks up
newly-installed packages within a second.

### Doom

```powershell
dotnet run --no-build --project Examples\Doom -p:Platform=x64
```

Compiles `doom_main.c` (PureDOOM) to RV32I ELF at startup and runs it
bare-metal with a real `doom1.wad`.

### Package feed

`Examples.Linux.Packageserver` cross-builds any of ~2700 buildroot packages
and serves them over HTTP. Inside the guest terminal: `rvpkg update`, then
`rvpkg install <name>`.

Other demos — `Runner`, `Video`, `Voxel`, `Sound`, `Input`, `TinyCC` — and
the full option reference are documented in
[`RiscVEmulator-RV32I/README.md`](RiscVEmulator-RV32I/README.md).

---

## ISA Support

| Extension | Status | Notes |
|-----------|--------|-------|
| **RV32I** | ✅ all 40 base instructions | `FENCE` retires as a NOP |
| M (mul/div) | ❌ traps | lowered to `__mulsi3`/`__divsi3` libcalls |
| A (atomics) | ❌ traps | patched away — single-hart, no-`A` sequences |
| F / D (float) | ❌ traps | software float via `softfloat.c` in the guest |
| C (compressed) | ❌ | every instruction is 32-bit |
| Zicsr / Zifencei | ❌ | no CSRs at all — trap-frame page at `0x0F000000` instead |
| Privileged M/U | ⚙️ via trap frame | `ECALL`/`EBREAK` + timer/external interrupts; no `MRET` — return via the `0xFFFF0004` gateway |
| MMU | ❌ | bare guest-physical addressing; no memory protection |

Anything outside the base ISA is an illegal instruction. That constraint —
held absolutely — is the whole point.
