# PERF_MOTT — L2-resident working set (cudaMemAdvise + prefetch)

## Idea

A single GPU lane interpreting RV32IMA is **latency-bound**: every instruction
fetch and register touch is a dependent load, and the measured ~530-cycle stall
is a **DRAM round-trip**. SASS micro-opts, SMT and pipelining were already shown
not to help; the `switch`, branchless decode and the decode-cache were the only
single-core wins. The remaining lever is **placement**: if the hot working set
lives in device memory, the dependent load hits **L2 (~200 cyc)** instead of
fault-migrating from host DRAM across PCIe.

`cudaMallocManaged` pages start with **no physical residency**. The first device
touch faults them in over the bus, and under memory pressure they can migrate
back to host. Mott pins the working set device-side up front:

- `cudaMemAdviseSetReadMostly` + `SetPreferredLocation = device` on the **shared
  RO code image** (`g_code`) — it is never written by the guest, so it can be
  duplicated/pinned read-only device-side.
- `cudaMemAdviseSetPreferredLocation = device` on each core's **writable**
  RAM / trap page / peripheral page, plus the `CoreState[]` / `CoreMem[]` arrays.
- `cudaMemPrefetchAsync(..., device)` on all of the above **before** the launch,
  so the set is materialized in device memory instead of fault-migrating mid-run.

Framebuffer + PCM are intentionally **left unhinted**: they are large and
host-reconciled every launch, so pinning them device-side would only evict the
hot CPU working set.

This is a **pure placement hint** — it can never change a load/store result, so
RV32IMA stays **bit-for-bit identical** whether it is on or off.

## Changes

- `Native/rv32i_cuda.cu`
  - `g_l2advise` (default **ON**) + `g_l2advise_dirty`, `g_code_cap`.
  - `l2_advise_buf()` / `l2_advise_all()` helpers (advise + prefetch the whole
    CPU working set + code image; best-effort, failures swallowed).
  - Applied **lazily** in `cuda_rv32i_step_all` (only when dirtied by
    `set_l2advise` or `set_code`) so a many-launch run pays the cost once.
  - C-ABI toggle `cuda_rv32i_set_l2advise(int)` / `cuda_rv32i_get_l2advise()`.
    (`rv32i_cuda` exports via `__declspec(dllexport)`; no `.def` to update.)
- `Core/Cuda/CudaEmulator.cs` — `bool L2Advise { get; set; }` property
  (reads/writes the DLL global; default ON).
- `Examples_CUDA/CudaBench/Program.cs` — `Run(..., int l2=1)` plumbing + a new
  `[mott/L2-advise]` A/B section: single-core (compute+data) and multi-core
  (4096×256) baseline-vs-advised MIPS, each with a bit-exact verify-word gate
  that aborts on any mismatch.

## Predicted gain

- Single core: the win depends on how much of the hot loop was already L1/L2
  resident. A tight loop's code is already cached, so expect a **modest**
  single-core gain (0–15%); the prefetch mainly removes cold-start fault
  migration and prevents host-ward eviction.
- Many cores: the working set spans thousands of per-core RAMs and is real DRAM
  pressure. Pinning + ReadMostly-duplicating the shared code is where the
  **largest** gain is expected — **~1.1–1.4×** aggregate is plausible if the
  baseline was paying migration/eviction traffic. Bounded by L2 capacity: once
  the per-core RAMs exceed L2, the hint helps residency stability but not raw
  capacity.

## A/B instructions (orchestrator runs these — do NOT run on Daniel's box without care)

1. Build the native DLL + C#:
   `MSBuild RiscVEmulator.sln -p:Platform=x64` (or just `Native\rv32i_cuda.vcxproj`).
2. Run the bench:
   `dotnet run --no-build --project Examples_CUDA\CudaBench -p:Platform=x64`
3. Read the `[mott/L2-advise]` block:
   - Every row must print **BIT-EXACT** (verify words equal between off/on).
     A `MISMATCH!` aborts with exit code 1 — that would be a correctness bug.
   - Compare the `MIPS/core` (single) and aggregate `MIPS` (multi) `off` vs `on`
     rows; the `speedup` column is `on / off`.
4. Force baseline globally from C#: set `CudaEmulator.L2Advise = false` (or call
   `cuda_rv32i_set_l2advise(0)`); leave default (ON) for the optimized path.

Safety: multi-core A/B is capped at 4096 cores (≤256 MB) per the VRAM
oversubscription rule; budgets stay ≪2 s/launch (TDR-safe).
