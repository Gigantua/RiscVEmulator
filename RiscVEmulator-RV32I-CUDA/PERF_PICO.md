# PERF_PICO — state minimizer

## Idea

Round 1 proved a single GPU lane is latency-bound and the only real cure is
**occupancy** (more resident warps). Occupancy and how many cores you can launch
at a fixed VRAM budget are both gated by *per-core resident footprint*. PICO
shrinks that footprint to its minimum.

The dominant waste in the per-core state was `CoreState::soft_csr` — an **inline
4096-word (16 KiB) array**. It made `sizeof(CoreState)` ≈ 16,536 B, so the
densely-packed `g_state[]` array (synced at every launch/exit, ideally
L2-resident) was 16 KiB *per core*. Throughput guests (compute/bench) never
touch a single CSR, so this 16 KiB is pure dead weight on the hot path and on
the VRAM budget that caps how many cores can co-reside.

## What changed

1. **`Native/rv32i_jit_shared.cuh`** — `CoreState::soft_csr` changed from
   `uint32_t soft_csr[4096]` (inline, 16 KiB) to `uint32_t* soft_csr` (8 B
   pointer into a separately-allocated `SOFT_CSR_COUNT`-word backing store).
   `sizeof(CoreState)` drops from ~16,536 B to ~64 B (**≈256× smaller**). Added
   the named constant `SOFT_CSR_COUNT = 4096`.

2. **`Native/rv32i_cuda.cu`** — host allocator wires the backing store and adds
   the C-ABI toggle `cuda_rv32i_set_pico(int)` (default **ON**) plus
   `cuda_rv32i_get_pico()` and `cuda_rv32i_corestate_bytes()`:
   - PICO ON (default): one contiguous slab `nCores*SOFT_CSR_COUNT` words;
     each core's `soft_csr` points at its slice. Cold slab pages in lazily.
   - PICO OFF (baseline): a private `cudaMallocManaged` CSR buffer per core,
     reproducing the original per-core allocation layout for A/B.
   `shutdown` frees the slab (PICO) or the per-core buffers (baseline).

3. **`Core/Cuda/CudaEmulator.cs`** — `static bool PicoStateMin` (default true),
   applied via `cuda_rv32i_set_pico` immediately before `cuda_rv32i_init`, and a
   `CoreStateBytes` read-only property.

4. **`Examples_CUDA/CudaBench/Program.cs`** — new `[pico]` A/B section: prints
   pre-PICO vs post-PICO `sizeof(CoreState)`, cores that fit a 256 MiB
   state-array budget for each, a **1-core bit-exact check** (slab vs per-core
   CSR must yield the identical verify word), and — only under `--sweep` — an
   aggregate-MIPS A/B at 4096 cores.

## Why it is semantics-preserving (bit-exact)

`trap_system` still executes `cpu.g->soft_csr[fn]` with `fn` a full 12-bit
index (0..4095) into a `SOFT_CSR_COUNT`-word store, exactly as before. Every CSR
read/write touches the identical word it always did; a read before any write
still returns 0 (the store is zero-initialized in both modes). Moving the array
out-of-line and choosing slab-vs-per-core is an **allocation choice only** — it
never changes a computed value. Both DLLs (interpreter + JIT) `#include` the
shared header, so they agree on the new `CoreState` layout and index the same
managed array consistently.

The `cuda_rv32i_set_pico(0)` path reproduces the baseline CSR allocation; the
verify word is asserted equal to the PICO path (MISMATCH → CudaBench returns 1).

## Predicted gain

- `sizeof(CoreState)`: ~16,536 B → ~64 B (**~256×**). At a 256 MiB state-array
  budget that lifts the core ceiling from ~16k to ~4M (state array no longer the
  constraint — RAM/Periph become the limit, and 16 KiB/core of VRAM is freed for
  more cores).
- Per-launch state sync (`g_state[]` load at entry / store at exit) shrinks
  256×, and the small array stays L2-resident → cheaper boundary cost.
- Net: more cores resident at the same VRAM → more warps → the proven
  occupancy mechanism hides more latency. Expect aggregate-MIPS at high core
  counts to rise once the freed VRAM is spent on additional cores; estimate
  +5–15% aggregate at the VRAM-bound fill point, 0% regression at low core
  counts (CSR slab is cold and never touched by CSR-free guests).

## A/B instructions

Default ON. To force baseline: `CudaEmulator.PicoStateMin = false` (host) or
`cuda_rv32i_set_pico(0)` before init (native).

Build the solution, then:

```
dotnet run --no-build --project Examples_CUDA\CudaBench -p:Platform=x64 -- --sweep
```

Read the **`[pico]`** block:
- `sizeof(CoreState): pre-PICO … → PICO …` and the `cores fitting …` line show
  the footprint shrink.
- `bit-exact (1 core): baseline 0x… , pico 0x…  →  MATCH ✓` is the correctness
  gate (must be MATCH; the program returns 1 on mismatch).
- Under `--sweep`, the `mode / 4096-core MIPS / verify` rows compare aggregate
  throughput baseline vs pico with the bit-exact verify word per row.

Compare the `pico` MIPS row against the `baseline` MIPS row; the verify words
must be identical.
