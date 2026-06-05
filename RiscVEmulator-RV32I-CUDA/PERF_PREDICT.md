# PERF_PREDICT — Frequency-biased predicated fast path

## Idea

Single-lane GPU interpretation is latency-bound, but the per-instruction
dispatch still costs real work: `cpu_step` selects an arm of a ~13-way opcode
`switch` (ptxas lowers it to a compare ladder / jump table on `instr & 0x7F`),
then a nested `switch (f3)`. A handful of opcodes — **ADDI, ADD/SUB, LW, SW,
conditional BRANCH** — dominate the dynamic instruction count of every integer
guest. If we peel those into a short *predicated* prologue that runs **before**
the full switch and `return`s early on a hit, the common case never pays the
jump-table indirection.

Within a warp all 32 lanes run the same guest image, usually at the same PC, so
the opcode predicate is **uniform** — the ladder stays divergence-free (Bool's
lesson: predicated compare-and-select, no per-lane branch divergence). ptxas
folds the arithmetic arms into straight-line `iadd`/`isub`/`selp` with a single
predicated store, skipping the dispatch entirely.

## Fast-path design

A flag-gated block at the top of `cpu_step` (`Native/rv32i_jit_shared.cuh`),
after the standard field decode (rd/f3/f7/u1/u2/s1/s2/nextpc already computed):

```
if (g_fastpath_on) {
    isAddi = op==0x13 & f3==0
    isAdd  = op==0x33 & f3==0 & f7==0x00
    isSub  = op==0x33 & f3==0 & f7==0x20
    if (isAddi|isAdd|isSub) { regs[rd] = isSub ? u1-b : u1+b;  ... return NONE; }
    if (op==0x03 & f3==2)   { regs[rd] = mem_read<u32>(s1+i_imm);  ... return NONE; }
    if (op==0x23 & f3==2)   { mem_write<u32>(s1+s_imm, u2);        ... return NONE; }
    if (op==0x63)           { taken=...; nextpc = taken?pc+b_imm:pc+4; ... return NONE; }
}
switch (op) { ...full ISA... }
```

Each arm reproduces the *exact* body of the switch `case` it shortcuts, then
performs the same epilogue the switch does (`regs[0]=0; pc=nextpc; return
{EXC_NONE,0}`). The predicates use bitwise `&`/`|` (no short-circuit branches)
so the conditions evaluate as predicated selects.

- **Flag**: `__device__ int g_fastpath_on = 1;` (default ON) defined in the
  shared header *inside* `#ifndef RVJIT_MEM_ONLY`, next to `cpu_step`. It is
  defined (not `extern`) there deliberately: BOTH modules that compile
  `cpu_step` — the interpreter DLL (`rv32i_cuda.cu`) and the runtime JIT DLL
  (RvJitRuntime's generated `.cu`, whose `jit_interp_step`→`cpu_step` references
  it) — need a local definition, and an `extern` would leave the JIT DLL with an
  unresolved `__device__` symbol. The JIT *part* files set `RVJIT_MEM_ONLY` and
  skip `cpu_step`, so exactly one TU per module defines it (no ODR clash). Host
  sets the interpreter DLL's copy via `cuda_rv32i_set_fastpath(int)` →
  `cudaMemcpyToSymbol`. The symbol persists across init/shutdown (module state,
  not managed memory).
- **OFF (`g_fastpath_on==0`)** skips the whole ladder → the switch runs
  unchanged → **bit-for-bit identical to baseline.**

## Per-opcode equivalence proof

For each fast-path arm, compared against the switch `case` in the same file:

| Op | Encoding gate | Fast path | Switch arm | Equivalent? |
|----|---------------|-----------|------------|-------------|
| ADDI | `op==0x13 & f3==0` | `regs[rd]=u1 + (uint32_t)i_imm` | case 0x13 f3=0: `(uint32_t)(s1+imm)` | Yes — unsigned add of `u1` and sign-extended `imm` has the identical bit pattern to two's-complement `s1+imm`. |
| ADD | `op==0x33 & f3==0 & f7==0` | `regs[rd]=u1+u2` | case 0x33 (non-M, non-illegal) f3=0,f7≠0x20: `(uint32_t)(s1+s2)` | Yes — same bit pattern. f7==0x00 is the only ADD encoding. |
| SUB | `op==0x33 & f3==0 & f7==0x20` | `regs[rd]=u1-u2` | case 0x33 f3=0,f7==0x20: `(uint32_t)(s1-s2)` | Yes — unsigned subtract equals two's-complement subtract bit-for-bit. f7==0x20,f3==0 passes the switch's `!(f7==0x20&&(f3==0||f3==5))` illegal filter (it is legal). |
| LW | `op==0x03 & f3==2` | `regs[rd]=mem_read<u32>(s1+i_imm)` | case 0x03 f3=2: identical | Yes — byte-identical, same `mem_read<uint32_t>`. |
| SW | `op==0x23 & f3==2` | `mem_write<u32>(s1+s_imm, u2)` | case 0x23 f3=2: identical | Yes — same address calc (`s_imm`) and `mem_write<uint32_t>`. |
| BRANCH | `op==0x63` (all f3) | same `switch(f3)` taken-table; `nextpc=taken?pc+b_imm:pc+4` | case 0x63: identical switch + same default | Yes — copied verbatim. Illegal f3∈{2,3} fall to `taken=0`→`pc+4` in BOTH (RV32I BRANCH never traps on bad f3 here). |

Epilogue equivalence: the switch sets `cpu.regs[0]=0; cpu.pc=nextpc; return
{EXC_NONE,0}` once at the bottom. Every fast-path arm performs the same two
writes and returns `{EXC_NONE,0}`. SW/BRANCH write no `rd`; setting `regs[0]=0`
is the same x0-sink the switch applies unconditionally → harmless and identical.

**Crucially, nothing illegal is shortcut.** OP-IMM shift sub-functions with bad
f7 (`f3==1/5`), OP M-extension (f7==0x01), OP illegal f7, and all byte/half
loads/stores are NOT matched (the gates require the exact common encoding), so
they fall through to the switch and trap/behave exactly as before. The fast
path can only ever return a result the switch would also return.

## Predicted gain

Dispatch is a small fraction of single-core cost (which is dominated by the
dependent instruction-fetch load latency), so the win is modest but real:
removing the opcode jump-table compare ladder + nested f3 switch for ~70-85% of
dynamic instructions. Estimate **+5–12% single-core MIPS** on integer-heavy
guests (`compute`, `data`), in line with the proven "+12% from branchless hot
path removes divergence" result — this is the same mechanism applied to the
top-5 opcodes. Memory-bound guests gain least (LW/SW still pay the load
latency); arithmetic/branch loops gain most.

## A/B instructions

Build the native DLL + CudaBench, then run CudaBench (orchestrator measures —
do NOT run on Daniel's box per the VRAM-hang note; budgets here are ≤300k
steps / 1 core so they are TDR-safe regardless):

```
# Build (orchestrator):
nvcc rv32i_cuda.vcxproj  (MSBuild, Platform=x64)
dotnet build Examples_CUDA/CudaBench

# Run — the [predict] section A/Bs OFF vs ON automatically:
dotnet run --no-build --project Examples_CUDA/CudaBench
```

The `[predict]` table prints, per guest, MIPS/core for fastpath OFF and ON, the
ON/OFF speedup, and the verify word for each. The bit-exactness gate asserts
`verify(OFF) == verify(ON)` for both guests and returns exit code 1 on mismatch.

Programmatic toggle: `cuda_rv32i_set_fastpath(0|1)` (exported C ABI; also wired
into `Core/Cuda/CudaEmulator.cs`). Default is ON.

## Files touched

- `Native/rv32i_jit_shared.cuh` — predicated fast path in `cpu_step` + `g_fastpath_on` definition.
- `Native/rv32i_cuda.cu` — `cuda_rv32i_set_fastpath` host toggle (cudaMemcpyToSymbol).
- `Core/Cuda/CudaEmulator.cs` — DllImport for the toggle.
- `Examples_CUDA/CudaBench/Program.cs` — `[predict]` OFF/ON A/B + verify gate.
