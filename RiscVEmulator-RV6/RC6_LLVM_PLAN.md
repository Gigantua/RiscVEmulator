# Plan: patch clang to emit RC6 directly

> Companion to `rv6.md` (the 6-op ISA), `TOOLCHAIN.md` (the self-built LLVM
> at `~/llvm-rv32i`), and the existing `Native/rv32i_core.cpp`.

## End state

- **Emulator**: only the 6 base ops (`ADD`, `XOR`, `ROL`, `BNE`, `LW`, `SW`)
  plus the trap unit. The current synthesis macros (`NOT`, `NEG`, `SUB`,
  `AND`, `OR`, `SLL`, `SRL`, `SRA`, `SLT`, `SLTU`, `LB`, `LH`) and the
  whole RV32I decoder are **deleted**. The execute loop is a 6-case switch.
- **Toolchain**: `~/llvm-rv32i/bin/clang -march=rc6` produces ELFs whose
  every instruction word is one of the 6 ops. lld accepts them; no
  relocations rely on banned instructions.
- **Net**: every macro currently in `RC6::AND` / `RC6::OR` / `RC6::SLL` /
  … lives **in the compiler backend instead of the emulator.** The
  compiler-emitted instruction stream is exactly what the silicon would
  execute.

This is the whole point of unifying on a self-built LLVM (`TOOLCHAIN.md`):
the patches that make this happen land in our LLVM fork.

## What "RC6" means as a binary ISA

RC6 is a **strict subset of RV32I's instruction encoding**, so we never
need to invent a new opcode space, ELF e_machine, or relocation type. The
existing RV32I encoder/decoder/disassembler in LLVM works as-is.

| Allowed (the 6) | RV32I encoding |
|---|---|
| `ADD rd, rs1, rs2` / `ADDI rd, rs1, imm12` | OP / OP-IMM funct3=0 |
| `XOR rd, rs1, rs2` / `XORI rd, rs1, imm12` | OP / OP-IMM funct3=4 |
| `ROL rd, rs1, rs2` / `RORI rd, rs1, imm5` (Zbb) | OP funct7=0x60 funct3=1; OP-IMM funct7=0x60 funct3=5 |
| `BNE rs1, rs2, off13` | BRANCH funct3=1 |
| `LW[B|H|W][U?] rd, off12(rs1)` | LOAD all five widths |
| `SW[B|H|W] rs2, off12(rs1)` | STORE all three widths |

Everything else in RV32I — `AND`/`OR`/`SLL`/`SRL`/`SRA`/`SUB`/`SLT`/`SLTU`/
their I-forms, `LUI`/`AUIPC`/`JAL`/`JALR`/`BEQ`/`BLT`/`BGE`/`BLTU`/`BGEU` —
is **banned at codegen**. Compiler must synthesize each from the 6.

`FENCE`, `ECALL`, `EBREAK` stay as today (decode-only NOPs / always-trap).

## What moves into LLVM

| Today, in `Native/rv32i_core.cpp` macros | Tomorrow, in LLVM patch |
|---|---|
| `NOT(a) = XOR(a, 0xFFFFFFFFu)` | Pseudo `NOT` lowered in ISel; constant materialized via pool |
| `NEG(a) = ADD(NOT(a), 1)` | ISel lowering |
| `SUB(a,b) = ADD(a, NEG(b))` | ISel pattern for `sub` |
| `AND(a,b)` via `(a+b)-(a^b)` + bit-31 fix | ISel custom lowering for `and` |
| `OR(a,b) = XOR(XOR(a,b), AND(a,b))` | ISel pattern |
| `SLL/SRL/SRA` via ROL + mask | ISel custom lowering; or pseudo expanded post-RA |
| `SLT/SLTU` via subtract + bit-31 | ISel custom lowering |
| `LB/LH` via `LW(addr,w) << k >> k` | TableGen pattern |
| (constants > 12 bits, currently inline `LUI`) | `.rodata.pool` + gp-relative LW |
| (no `JAL`/`JALR` in RC6) | Either keep them as honorary ops, **or** SMC trampoline — open question |

## Phased plan

### Phase 0 — verifier + baseline (no LLVM changes)

1. Write an **RC6 verifier**: walks an ELF and reports every instruction
   not in the allowed set. Output is `function:offset:hex:mnemonic`.
   Implementation: small C# tool in `Examples/` or an llvm-objdump wrapper.
2. Run it against the current `Runtime/runtime.c`, `Tests/Programs/*.c`,
   and the Linux kernel. The output is the gap to close — concrete count
   of `lui`, `auipc`, `and`, `or`, `slli`, `bge`, `jalr` instances per
   binary.
3. Add an **RC6 trace mode** to the emulator: when `RVEMU_RC6_STRICT=1`,
   `cpu_step` rejects (`EXC_ILLEGAL`) any opcode outside the 6. Run the
   test suite under it; expect failures everywhere — that's the unblock
   list for Phase 2.

Output: a single number ("today the test suite hits N non-RC6 ops in M
distinct sites") that goes down with each subsequent phase.

### Phase 1 — encoding + assembler

No work needed. RC6 ⊂ RV32I so the existing LLVM RV32I assembler/encoder
handles every RC6 instruction. The verifier from Phase 0 is what enforces
the subset.

### Phase 2 — restrict LLVM codegen

The smallest viable LLVM patch.

1. Add a target feature `+rc6-only` to `RISCVSubtarget.h/.cpp`. Wire it
   into `-march=rv32i_zbb_rc6only` or a new `-march=rc6` alias.
2. In `RISCVISelLowering.cpp`:
   - For each banned operation (`ISD::AND`, `ISD::OR`, `ISD::SUB`,
     `ISD::SHL`, `ISD::SRL`, `ISD::SRA`, `ISD::SETCC` variants), call
     `setOperationAction(..., Custom)` when `Subtarget.hasRC6Only()`.
   - Implement custom lowering for each: emit the macro from rv6.md as
     a sequence of `ISD::ADD`/`ISD::XOR`/`ISD::ROTL` nodes.
3. In `RISCVInstrInfo.td`:
   - Predicate every `AndInst`/`OrInst`/`SubInst`/etc. pattern with
     `Predicates = [..., NotRC6Only]`.
4. Constants:
   - Disable `LUI`/`AUIPC` emission under `+rc6-only`. Add a new pseudo
     `RC6_LoadGP_Pool` that lowers to `LW rd, off(gp)` where the linker
     resolves `off` against a per-module `.rodata.pool` section.
   - This is the heaviest piece — touches `RISCVMCInstLower`,
     `RISCVAsmPrinter`, and a new relocation type. **~1 week of work
     on its own.**
5. Branches: every `BEQ`/`BLT`/`BGE`/`BLTU`/`BGEU` → custom lowering to
   `SLTU` + `BNE`, or `XOR` + `BNE` (for equality).
6. `JAL`/`JALR`: **open question** — see "Decision points" below. Easiest
   MVP keeps them as honorary RC6 ops.

Phase 2 milestone: `clang -march=rc6 -O2 Tests/Programs/primes.c -c` and
`rc6-verify primes.o` reports zero violations.

### Phase 3 — runtime + lld

1. `crt0_rc6.S`: hand-written RC6 assembly that initializes `gp` to the
   `.rodata.pool` base, sets up the stack, calls `main`, exits via MMIO.
2. Compiler-rt builtins (`__muldi3`, `__divsi3`, `__udivsi3`, …):
   rebuild for RC6. They're already pure shift+add per
   `Examples/Linux.Build_RV32i/board-patches/linux/0002-...`, so they
   should pass the verifier with minor tweaks.
3. lld: add the new relocation type for gp-relative pool loads. Should
   be additive — existing RV32I relocations keep working.

### Phase 4 — emulator drops the synthesizer

1. Add `core_rc6.cpp`: a copy of `rv32i_core.cpp` with the macro block
   deleted and `cpu_step` reduced to a 6-case switch.
2. Wire `-DRVEMU_RC6_ONLY` to compile this instead of the synthesizer.
3. Run the full test suite under it; any failure is a Phase 2 bug.
4. When green: **delete** the macro block from `rv32i_core.cpp` and the
   RV32I decoder cases. `RC6` keeps only the 6 base methods. The trap
   unit unchanged.

### Phase 5 — Linux kernel

**No kernel source patches needed.** Two reasons:

1. **C source is the bulk of the kernel** — every `a & b`, every `<<`,
   every signed compare is a C expression. The Phase 2 LLVM patches
   lower those to RC6 macros at codegen time. Just rebuild with
   `-march=rc6`; the compiler does all the work.

2. **Hand-written privileged `.S` (CSR ops, etc.) keeps using whatever
   it uses.** Those instructions are *already* not in our CPU's accept
   set — `csrr`/`csrw`/`mret` etc. all trap as illegal today and the
   trap unit handles the context switch (see PART 3 of
   `rv32i_core.cpp`). RC6 changes nothing here: the trap unit is the
   architectural mechanism for everything outside the executable
   instruction set, the kernel already relies on it, and that path
   carries the load. The compiler-emitted bulk is RC6; the
   hand-written privileged stubs are caught by the trap unit. Same
   shape as today's `csrr` → trap → trap-frame handler.

So the previous no-A/no-M kernel patches (`board-patches/linux/0001`–
`0004`) **are not the precedent for this step**. Those patches existed
because A/M instructions appear inside C-emitted code (`a *= b` etc.)
where they trap *inside the hot loop*, not at a clean syscall boundary.
RC6 is different: every C operator the compiler used to lower to a
banned instruction (`and`, `or`, `slli`, …) now lowers to a synthesized
sequence at codegen, so the compiled `.o` never contains the banned
opcode at all. Nothing to patch.

Phase 5 work:

1. Rebuild kernel with `Examples/Linux.Build_RV32i -- --toolchain=clang`
   after Phase 2 lands. The buildroot pipeline already routes through
   the self-built clang (`TOOLCHAIN.md`).
2. Boot it in `Examples/Linux`. Same userspace boot screen as today.
3. Run `rc6-verify` on `vmlinux` for sanity. Expect: clean across
   compiled `.o` files; remaining violations confined to known
   privileged-asm sites and harmless (they trap anyway).

Risk: low. Cost: a kernel rebuild plus a boot test.

## Decisions locked

1. **RC6 = 6 + JAL/JALR.** Control transfer keeps the architectural
   PC-write opcodes. SMC trampolines saved no gates and broke ROM execution.

2. **Constants via `.rodata.pool` + gp-relative LW.** `LUI`/`AUIPC` are
   banned. Cost: one extra memory load per materialized constant,
   amortized across uses.

3. **Width-parameterized `LW`/`SW` = 1 op or 3?**
   - Today the emulator counts them as 1 (with a mode field). LLVM
     would naturally expose them as 5 + 3 separate instructions.
   - For LLVM's purposes the width is just an encoding bit — it's
     genuinely 6 distinct instructions in the decoder. We've been
     calling it "1 op" for the headline count; LLVM doesn't care.

4. **MVP target: bare-metal Examples or also Linux kernel?**
   - Both get done in the same Phase 2 work. The compiler is the only
     thing that changes; the kernel is just another C codebase. After
     Phase 4 lands, Phase 5 is a rebuild + boot test.

5. **What's the first runnable milestone?**
   - **Recommend**: `Tests/Programs/primes.c` compiles + runs end-to-end
     on the RC6-only emulator, with `rc6-verify` passing on the linked
     ELF. About 200 LOC of test program; doesn't need the runtime
     library (uses only integer arithmetic + putchar).

## Cost rough estimate

| Phase | Effort | Risk |
|---|---|---|
| 0 — verifier + baseline | 1 day | trivial |
| 1 — encoding (no-op) | 0 | — |
| 2 — LLVM codegen restrict + custom lowering | 1–2 weeks | medium; AND/OR/shifts are well-trodden patterns |
| 2 — `LUI`/`AUIPC` removal + pool | 1 week | high; new relocation type, lld touches |
| 3 — crt0 + builtins + lld | 3 days | low |
| 4 — emulator switch | 1 day | low; mostly deletion |
| 5 — Linux kernel | 1 day | low; just rebuild + boot — no patches |

Total: **~5 weeks** to a working RC6-only stack, **bare-metal and Linux**.
Linux is essentially free once Phase 2 lands because the compiler does
the heavy lifting and the trap unit catches the rest.

## Why we're set up for this

- The toolchain is **already self-built** (`TOOLCHAIN.md`). LLVM patches
  land in `~/llvm-project`, rebuild via `build-llvm.sh`, and the whole
  Examples + Linux pipeline picks them up automatically.
- The synthesis macros **already exist** in `Native/rv32i_core.cpp`. The
  LLVM lowering patterns are direct ports — the algebraic identities
  are the same. We're moving them from runtime code into compile-time
  patterns.
- The emulator **already has the strict-mode hook** (Phase 0's
  `RVEMU_RC6_STRICT`) basically wired — `cpu_step`'s default case
  returns `EXC_ILLEGAL`. We just need to narrow the accept set.
- The Linux build **already disables every other extension** (no A, no
  M, no F, no C). RC6 is the next reduction along the same line.

## First action

Pick the answer to "6 vs 6+JAL/JALR" and "pool vs inline constants",
then write the Phase 0 verifier. Everything else flows from those two
decisions.
