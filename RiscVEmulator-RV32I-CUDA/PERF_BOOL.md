# PERF_BOOL — Branch-eliminator hot path (CUDA RV32I core)

Personality: **BOOL**. Branch: `cuda-perf-fleet` worktree.

## The change

Made the three data-dependent muxes in the CUDA interpreter hot path
(`Native/rv32i_jit_shared.cuh::cpu_step`) **branchless**:

1. **BRANCH condition select** (opcode `0x63`, the `switch (f3)` over
   BEQ/BNE/BLT/BGE/BLTU/BGEU) → `branch_taken_bl()`, a bit-decode of `f3`:
   - `eq = (u1==u2)`, `lt = (f3&4)? (u1<u2) : (s1<s2)`
   - `base = (f3&4)? lt : eq` (bit2 = LT-family vs EQ-family)
   - `taken = base ^ (f3&1)` (bit0 inverts → BNE/BGE/BGEU)
   The `if (taken) nextpc = …` was also turned into a `nextpc = taken ? tgt :
   nextpc` select (no control flow).

2. **OP-IMM ALU mux** (opcode `0x13`, `switch (f3)`) → `alu_bl()`.
3. **OP ALU mux** (opcode `0x33` non-M arm, `switch (f3)`) → `alu_bl()`.

   `alu_bl()` computes all eight arms (ADD/SUB, SLL, SLT, SLTU, XOR,
   SRL/SRA, OR, AND) unconditionally and index-selects the live one with a
   3-bit `?:` decode (each `?:` lowers to a PTX `selp`, no branch). Two
   separate flags pick the f7==0x20 variants inside the precomputed arms:
   `sub` (SUB-vs-ADD) and `sra` (SRA-vs-SRL). Crucially OP-IMM passes
   `sub=false` — ADDI is *always* an add regardless of the immediate's high
   bits (there is no SUBI), so only OP (`f3==0, f7==0x20`) sets `sub`.

4. **Illegal-funct checks** folded into single branchless bool predicates
   (`bad = (f3==1 & f7!=0) | (f3==5 & f7!=0 & f7!=0x20)`), so the common
   (legal) case is a straight-line predicate test and only the rare illegal
   instruction takes the cold `return {EXC_ILLEGAL,…}`.

The M-extension arm (`f7==0x01`), LOAD/STORE width switch, and the A-extension
`switch (f5)` were **left as switches** — they either have side effects
(memory) or are not the dominant hot mux, so converting them risks executing
speculative loads or buys little.

## Why it's bit-exact

Every replacement is the algebraic identity of the switch arm it replaces:

- `branch_taken_bl` reproduces all six branch predicates exactly (verified arm
  by arm; the two unused `f3` slots 2/3 never reach opcode `0x63` from a valid
  encoding and would have hit the old `taken=0` default — but RV32I has no
  BRANCH with f3∈{2,3}, identical to before).
- `alu_bl` returns the same `uint32_t` each `case` produced; SLT uses signed
  compare, SLTU unsigned, shifts use the same 5-bit `sh`, SRA uses the signed
  arithmetic shift exactly as before. For OP-IMM, `arg2u=(uint32_t)imm` and
  `arg2s=imm` match the baseline's `(uint32_t)imm` / `imm` operands.
- The illegal predicates are De Morgan / OR rewrites of the original `||`/`&&`
  conditions — same truth table.
- Computing all arms has **no observable side effects**: every arm is pure
  register arithmetic (no memory, no divide-by-zero — there is no DIV in this
  mux; the M arm is untouched). Speculatively computing an unused shift or
  compare is harmless.

A `BRANCHLESS` template parameter (default `true`) on `cpu_step` keeps the
original switch code compiled-in as the baseline for A/B. `do_step` selects it
via the `RVCUDA_BRANCHLESS` macro (default 1).

## Expected effect

The hot path is dominated by OP/OP-IMM/BRANCH. ptxas previously lowered the
nested `switch (f3)` to a jump table / chained-branch sequence; within a warp,
lanes running the same opcode but different `f3` (very common — a basic block
mixes ADD/SLLI/XOR/branches) **diverge** on that inner switch, serializing the
arms. The branchless form has all lanes execute one straight predicated
sequence → no divergence on `f3`, and the `nextpc` select removes the
BRANCH-taken divergence too. Net: fewer dynamic branches, no `f3`-driven
divergence, modest SASS-instruction reduction on the ~110-instr/step budget.
Realistic expectation: small single-digit-percent throughput gain on
branch/ALU-heavy workloads (bench/primes/crc), neutral on memory-bound ones.
It is primarily a *divergence* win, not an instruction-count win.

## How to measure

A/B with the macro (orchestrator builds + runs serially — do NOT run on the
shared box yourself):

```
# baseline (switch):
nvcc ... -DRVCUDA_BRANCHLESS=0 Native/rv32i_cuda.cu ...
# branchless (default):
nvcc ... Native/rv32i_cuda.cu ...
```

1. **Correctness gate first**: `CudaPeriphTest` (22 checks) + the bench
   correctness gate must pass identically for both builds (they must, since
   the paths are bit-exact).
2. Run the CUDA bench (bench/primes/matmul/crc/softfp) under both builds,
   best-of-3, compare MIPS / kernel time. Optionally diff SASS
   (`cuobjdump -sass`) for the `do_step`/`cpu_step` region to confirm the
   `f3` jump table is gone and branch count dropped.
3. If neutral-or-worse on every workload, flip the default macro back to 0;
   the baseline switch is still the compiled fallback.

## Files touched

- `Native/rv32i_jit_shared.cuh` — `branch_taken_bl`, `alu_bl`, templated
  `cpu_step<BRANCHLESS>`, branchless illegal predicates + BRANCH/OP/OP-IMM
  rewrites.
- `Native/rv32i_cuda.cu` — `RVCUDA_BRANCHLESS` macro; `do_step` passes it to
  `cpu_step`.
