# RISC-V Emulator — three cores, one design

> ## 🌍 The world's first Linux to boot on a *bare* RV32I core.
>
> No `M`, no `A`, no `F`, no `C` — **not even `Zicsr`**. A real Linux 6.6
> kernel, a graphical desktop, and Doom, all running on the 40 base integer
> instructions of RV32I and nothing else.

![Linux](https://img.shields.io/badge/boots-Linux%206.6-brightgreen)
![Alpine](https://img.shields.io/badge/boots-Alpine%20riscv64-0d597f)
![Doom](https://img.shields.io/badge/runs-Doom-red)
![TinyCC](https://img.shields.io/badge/compiles-C%20in%20TinyCC-blue)
![.NET 10](https://img.shields.io/badge/.NET-10-blueviolet)

Most "RISC-V Linux" quietly assumes **RV32IMA** — multiply, divide and
atomics in hardware. The canonical tiny emulator is even *named* for them:
`mini-rv32ima`. This repository goes the other way. It strips the CPU down to
the **pure RV32I base integer ISA** — no multiply, no atomics, no float, no
compressed encodings, and no control-and-status registers at all — and *still*
boots a real Linux kernel to a windowed Microwindows desktop.

To make that work, the entire image — kernel, uClibc-ng, busybox, every
package — is rebuilt so that not a single `mul`, `div`, `rem`, `amo`, `lr`,
`sc` or float opcode is ever emitted; multiply and divide lower to pure
shift-and-add libcalls. And because there is no `Zicsr`, there are no CSRs:
traps spill the register file into a memory-mapped **trap-frame page** and
return through a fixed resume gateway, since the base ISA has no `MRET`.

That is the punchline. The rest of the repository is the ladder that leads up
to it and the 64-bit world that grows out of it.

---

## The three cores

This is a monorepo of **three sibling emulators**. They share one design — a
single-file C++ CPU hot path wrapped in a thin C# peripheral shell — and form
a deliberate capability ladder from the most minimal RISC-V that can run Linux
to a full 64-bit machine running a real distribution.

| Folder | CPU ISA | Privilege / trap model | Boots | Flagship demo |
|--------|---------|------------------------|-------|---------------|
| [`RiscVEmulator-RV32I`](RiscVEmulator-RV32I/) | `RV32I` — base only | **No CSRs.** Memory-mapped trap-frame page; `0xFFFF0004` resume gateway | Linux 6.6 nommu | Microwindows desktop + Doom |
| [`RiscVEmulator-RV32IMA_Zicsr`](RiscVEmulator-RV32IMA_Zicsr/) | `RV32I` + `Zicsr` + `Zifencei` | Architectural CSRs; M/S/U modes; trap delegation; `MRET`/`SRET`/`WFI` | Linux 6.1 nommu | bare-metal Doom + TinyCC |
| [`RiscVEmulator-RV64GC`](RiscVEmulator-RV64GC/) | `RV64GC` — `IMAFDC` | Full M/S/U; **Sv39 MMU**; SBI firmware shim | Alpine Linux `riscv64` | IceWM/XFCE desktop, `apk` |

Each folder is a **self-contained Visual Studio solution** (`RiscVEmulator.sln`)
with its own native core, peripherals, examples and tests. Pick a folder, open
its solution, build, run — they do not depend on each other.

### `RiscVEmulator-RV32I` — the bare-metal world first

The hero. A pure integer datapath: 40 RV32I instructions, ~355 lines of core
C++, and nothing else. Anything outside the base ISA — a `SYSTEM` opcode, an
`M`/`A`/`F` encoding — is handed back to the surrounding trap unit as an
exception. There are **no architectural CSRs**: `csrr`/`csrw`/`mret`/`wfi` all
trap as illegal. Traps work by spilling `epc` + `x1..x31` + `status`/`tval`/
`cause` into a fixed 36-word landing pad whose layout *is* the Linux
`struct pt_regs`, so the kernel's own trap frame is the hardware trap frame —
no translation copy.

It boots a real Linux 6.6 nommu kernel to a graphical **Microwindows nano-X
desktop** — taskbar, terminal (`sh -i` over a real pty with VT100 parsing),
clock, chess, tetris, calculator, and a windowed `doomgeneric`. A self-hosted
package feed (`rvpkg` + `Examples.Linux.Packageserver`) cross-builds and
installs ~2700 buildroot packages. → [full README](RiscVEmulator-RV32I/README.md)

### `RiscVEmulator-RV32IMA_Zicsr` — the architectural-privilege build

The same RV32I integer core, but with the **standard RISC-V privileged
architecture** restored: real `Zicsr` CSRs (`mstatus`, `mtvec`, `mepc`,
`medeleg`/`mideleg`, `satp`, …), M/S/U modes, trap delegation, and
`MRET`/`SRET`/`WFI`. This is the conventional trap model — the one a stock
`mini-rv32ima`-class kernel expects — and it is what makes this folder a
separate variant from the trap-frame-page base core above.

*(Naming note: the `_Zicsr` suffix is the operative distinction. The integer
datapath is still RV32I — `M` lowers to libcalls and `A`/`F` opcodes trap; the
`IMA` in the folder name reflects the privileged-spec lineage and the
`mini-rv32ima` image family it boots.)* → [full README](RiscVEmulator-RV32IMA_Zicsr/README.md)

### `RiscVEmulator-RV64GC` — the 64-bit, MMU, real-distro build

The full machine. A single-file `RV64GC` interpreter — `IMAFDC` + `Zicsr` +
`Zifencei` — with a three-level **Sv39 MMU** (superpages, hardware A/D bits,
256-entry TLB) and an **SBI firmware shim**, so the kernel boots directly in
S-mode. It boots a genuine, unmodified **Alpine Linux `riscv64`** userland,
runs a windowed **IceWM/XFCE desktop**, installs software with the real `apk`
package manager from ~10 000 official Alpine packages, has audio, networking
and a persistent 3 GiB virtio-blk disk, and still runs Doom and TinyCC.
→ [full README](RiscVEmulator-RV64GC/README.md)

---

## One design, three times

All three cores are built the same way — that shared architecture is the
reason a 40-instruction CPU and a full RV64GC machine can live in one repo:

```
Emulator (C# P/Invoke shell)
  │  reserves the guest's physical address space on the host up front,
  │  hands the base pointer to the native DLL — both sides share the VA range
  │
  ├── rv32i_core.dll / rv64gc_core.dll   (C++ hot path — ClangCL)
  │     entire CPU state in one CPU_State struct; do_step(CPU_State&)
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
  └── ElfLoader   PT_LOAD segments / RISC-V flat Image header
```

- **Native single-file CPU** — the step loop is one `switch` statement in one
  C++ file, compiled with the ClangCL toolset, linked with no CRT.
- **Zero-copy memory** — the C# host reserves the guest address space and the
  native CPU dereferences it directly. No translation, no per-access dispatch.
- **MMIO via Windows VEH** — guarded pages fault; a vectored exception handler
  turns the fault into a peripheral call. The CPU has zero MMIO awareness.
- **C# is just the shell** — peripherals (UART, framebuffer, keyboard, mouse,
  audio, RTC, CLINT, PLIC, virtio-net/blk), the SDL2 frontend, the ELF loader
  and the test harness are all managed code.

---

## Build & run

Each folder builds independently. Pick one, then:

```powershell
# Example: the bare RV32I core
cd RiscVEmulator-RV32I

# Build the C++ core + every C# project
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    RiscVEmulator.sln -p:Platform=x64

# Run a demo (bare-metal Doom — no WSL needed)
dotnet run --no-build --project Examples\Doom -p:Platform=x64
```

**Prerequisites** (host is Windows-only):

| Tool | Purpose |
|------|---------|
| [.NET 10 SDK](https://dotnet.microsoft.com/download) | Build and run the C# projects |
| Visual Studio 2022 / 2026 + **C++ workload + Clang/LLVM** | Build the native core (ClangCL toolset) |
| [LLVM/Clang](https://releases.llvm.org/) + `lld` in `PATH` | Cross-compile bare-metal RISC-V ELF guests |
| **WSL2 + Ubuntu** | Only for building the Linux images (buildroot runs in WSL) |

See each folder's `README.md` for the full, variant-specific build and run
instructions, and its `CLAUDE.md` / `AGENTS.md` for design notes.

---

## Which one do I want?

- **Want the headline?** → [`RiscVEmulator-RV32I`](RiscVEmulator-RV32I/) —
  Linux on a CPU with literally nothing but the base integer ISA.
- **Want the textbook RISC-V privileged model** (CSRs, `mtvec`, `MRET`, trap
  delegation)? → [`RiscVEmulator-RV32IMA_Zicsr`](RiscVEmulator-RV32IMA_Zicsr/).
- **Want a real desktop distro, an MMU and a package manager?** →
  [`RiscVEmulator-RV64GC`](RiscVEmulator-RV64GC/).

All three play Doom. All three run a C compiler inside the emulator. Only one
of them does it on a bare RV32I core — and that one had never been done before.
