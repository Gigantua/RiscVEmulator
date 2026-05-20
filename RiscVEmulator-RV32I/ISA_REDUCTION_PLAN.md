# ISA Reduction Plan — Shrinking Below Base RV32I

> Goal: get the toolchain to emit a *subset* of base RV32I so the CPU core
> (and the eventual FPGA synthesis target, see `FPGA_SYNTHESIS.md`) can drop
> instruction decoders entirely. Companion to `CLAUDE.md`.

## TL;DR

Base RV32I is 37 compute instructions + FENCE/ECALL/EBREAK. M/A/F are already
gone. This plan goes *below* RV32I. There is **no clang flag** for a
sub-RV32I `-march` — `rv32i` is the floor of the standard profiles. So
reduction happens in three tiers of increasing effort:

| Tier | Mechanism | Removes | Effort |
|------|-----------|---------|--------|
| 1 | Compiler/linker flags + verification | AUIPC, FENCE, FENCE.I, EBREAK, (ECALL for examples) | none — mostly already true |
| 2 | Forked LLVM **and** GCC RISC-V backends | LB/LBU/LH/LHU/SB/SH, SLT/SLTI, SRA/SRAI | maintain two forks (clang for examples, GCC for kernel) |
| 3 | Post-`.o` / post-link instruction rewriting | same as Tier 2, no fork | one toolchain-agnostic binary-rewriter — **preferred** |

## Toolchain reality — there are TWO compilers

| Target | Compiler |
|--------|----------|
| Examples, tests, native CPU DLL | **LLVM/clang** (`C:\Program Files\LLVM`, ClangCL) |
| Linux kernel + all guest userspace | **GCC** — buildroot's own `riscv32-buildroot-linux-uclibc-gcc` 13.3.0 |

Consequence: Tier-1 flag changes (`-mcmodel`) apply to both (GCC and clang
share the flag). But Tier 2 — a backend that lacks instructions — would have
to be implemented **twice** (an LLVM fork *and* a GCC fork) and kept in sync.
This is why **Tier 3 (post-link rewriting) is preferred for sub-base
reduction**: a binary rewriter is toolchain-agnostic — it does not care
whether GCC or clang emitted the `.o`. One rewriter covers both targets;
two compiler forks do not.

(Alternative: unify on LLVM by building the kernel + userspace with clang via
buildroot's external-toolchain path. RV32 nommu Linux under clang is not
well-trodden — treat as a research spike, not a plan dependency.)

## Step 0 — Measure what is actually emitted (do this first)

Static disassembly is **unreliable here**: Doom/TinyCC embed read-only data
inside `.text`, so `llvm-objdump -d` desyncs and prints spurious
`csrrsi`/`<unknown>`/hex-byte "instructions". Two trustworthy methods:

1. **Static, clean** — disassemble the `.o` files (rodata is a separate
   section there) or feed `.text` through the emulator's own decoder walking
   only bytes covered by `STT_FUNC` symbols.
2. **Dynamic (recommended)** — instrument `cpu_step` in `rv32i_core.cpp` with
   a `static uint64_t opcount[...]` bumped per retired instruction, add an
   `rv32i_dump_opcounts()` export, and run a full Doom / Linux-boot session.
   Dynamic counts are ground truth: "AUIPC retired 0 times in a full Doom
   run" *proves* it is safe to delete the decoder. Static analysis cannot
   distinguish dead code from live.

**Build a dynamic opcode profiler before removing anything.** It is ~20 lines
and turns every claim below into a verified fact.

## Tier 1 — Free removals (flags + verification only)

A noisy histogram of `doom.elf` / `voxel.elf` / `tinycc_demo.elf` already
shows **zero `auipc`, `fence`, `ecall`, `ebreak`** anywhere. Confirm with the
Step 0 profiler, then physically drop these decoders:

### AUIPC
- Only emitted for PIC or `-mcmodel=medany`. On **RV32**, `medlow` (LUI+ADDI
  absolute) reaches the entire 4 GiB address space — there is no medany
  benefit on a 32-bit target.
- **Examples**: compiled non-PIC at fixed addresses → already AUIPC-free.
  Lock it in: ensure `-fno-pic -mcmodel=medlow` (clang's RV32 default, but
  make it explicit in every `Examples/*/Program.cs` clang arg list) and keep
  linker relaxation on so `call` pseudos relax to `jal`.
- **Linux**: the RV32 kernel is normally built `-mcmodel=medany` (see
  `arch/riscv/Makefile`) → it *does* emit AUIPC. Action: add a Build_RV32i
  kernel patch rewriting the kernel `-mcmodel` to `medlow`, then verify with
  the dynamic profiler on a full boot. This is the one real Linux work item.

### FENCE / FENCE.I
- Examples emit none. The Build_RV32i kernel patches already rewrite kernel
  `fence`/`fence.i` → `nop`. The core already retires FENCE as NOP.
- Action: once the profiler confirms 0 dynamic executions, delete the `0x0F`
  case — an unexpected FENCE then traps illegal (acceptable, single-hart).

### EBREAK
- Emitted only by `__builtin_trap` / `abort` / UBSan. Examples emit none.
- Action: stop decoding EBREAK specially in `trap_system`. It folds into the
  existing illegal-instruction trap path — one fewer decode case, identical
  observable behavior for a debugger-less bare-metal core.

### ECALL — examples only
- Bare-metal examples exit via MMIO `0x40000000` and never `ecall`.
- **Linux requires ECALL** for syscall entry — it cannot be removed there.
- Conclusion: ship **two core profiles** — `RV32I_examples` (no ECALL) and
  `RV32I_linux` (ECALL kept). The FPGA bitstream picks one.

**Tier 1 result:** a documented `RV32I-` profile = RV32I minus
{AUIPC, FENCE, FENCE.I, EBREAK} (and ECALL for the examples profile),
achieved with *zero* codegen penalty and essentially no compiler work — the
compiler already cooperates.

## Tier 2 — Forked LLVM backend (the real sub-RV32I core)

To remove instructions the compiler genuinely *wants* to emit, the backend
must be told they do not exist; LLVM's legalizer then synthesizes them from
the survivors. Add a private subtarget feature (e.g. `+xrvemu-min`) in
`RISCVFeatures.td` and gate code generation on it:

| Drop | LLVM change | Synthesized as | Penalty |
|------|-------------|----------------|---------|
| LB/LBU/LH/LHU | `setLoadExtAction(_, i8/i16, Expand)` | LW + shift + mask/sign-extend | ~3 ops per sub-word load |
| SB/SH | `setTruncStoreAction(i32, i8/i16, Expand)` | LW + mask + OR + SW (read-modify-write) | ~5 ops per sub-word store; **not atomic** — fine single-hart |
| SLT/SLTI (signed) | `setOperationAction(ISD::SETLT, Expand)` keep SLTU | flip sign bits (`xor 0x80000000`) then SLTU | ~3 ops |
| SRA/SRAI | `setOperationAction(ISD::SRA, Expand)` | SRL + sign-fixup mask | ~3 ops |

Trade-off is steep: Doom alone has thousands of `sb`/`lbu`; expanding them
bloats code and slows execution. **Recommendation:** only pursue the sub-word
load/store removal if FPGA LUT pressure from the misalign-capable byte/half
memory unit justifies it. SLT/SRA removal saves little gate area — skip.

Cost: maintaining an LLVM fork and rebuilding clang. The byte/half store
expansion is the only one that meaningfully shrinks the datapath (removes the
byte-enable / sub-word write logic).

## Tier 3 — Post-link rewriting (no LLVM fork)

The repo already does same-size rewriting (`fence`→`nop`, no-A/no-M libcalls
in Build_RV32i). Extend that idea: a binary-rewriter pass over `.o` files
(before linking, so relocations are still symbolic) that expands the
to-be-removed instructions into kept-instruction sequences.

- 1:1 same-size rewrites (FENCE→NOP) are trivial and already done.
- N:1 expansions (LB → LW+shift+mask) change code size → shift every
  subsequent label → must run **before link** on `.o` files and let the
  linker re-resolve, or do a full relaxation pass.
- This avoids an LLVM fork at the cost of writing a relocation-aware
  RV32I→RV32I- translator. Comparable effort to Tier 2; pick one.

## Recommended path

1. **Now**: build the dynamic opcode profiler (Step 0). ~20 lines.
2. **Now**: run it on Doom, Voxel, TinyCC, and a full Linux boot. Confirm the
   Tier-1 set is dynamically dead.
3. **Now**: make `-fno-pic -mcmodel=medlow` explicit in every example's clang
   invocation; add the kernel `medlow` patch to Build_RV32i; delete the
   AUIPC/FENCE/EBREAK decoders; split the core into `examples` / `linux`
   profiles (ECALL gated).
4. **Later, only if FPGA area demands it**: Tier 2 byte/half store removal via
   an LLVM subtarget feature. Everything else (SLT/SRA) is not worth the area.

## Why not just a smaller `-march`?

`rv32i` is the smallest ratified profile. `rv32e` only halves the register
file (32→16 regs); it does **not** remove instructions and the ilp32e ABI is
poorly supported. There is no standard "RV32I minus X" — sub-RV32I is
inherently a custom-core exercise, which is why Tiers 2/3 require either an
LLVM fork or a binary rewriter.
