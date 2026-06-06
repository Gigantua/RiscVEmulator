# Benchmark — the CUDA RV32I core perf referee

This is the **frozen, representative RV32IMA workload** used to measure the CUDA
RV32I emulator core. It is the **referee for all CUDA-core perf changes**: every
optimization (core, JIT, scheduler, memory) is judged by running this benchmark
before and after and comparing the MIPS *and* the two correctness signatures.

## What it is

`benchmark.elf` + `benchmark.dat` are loaded into a 16 MB `CudaEmulator`
(sp `0x9FFF00`, WAD base `0xA00000`, WAD-size word at `0x9FFFFC`). The guest is a
real branchy, indirection-heavy program (data-driven init + render over the full
16 MB working set) — i.e. the honest ~4 MIPS regime, **not** a tight ALU
microbenchmark that flatters the number.

`DeterministicTime=true` derives the guest clock (RTC + CLINT `mtime`) from the
cumulative step count instead of the host wall clock, so a fixed step budget
executes a **bit-for-bit reproducible** instruction stream run-to-run. That is
what makes it a clean A/B and what makes the state-hash a stable regression
signature.

The run does a 1M-step warm-up (untimed: ramps GPU clocks + pays the cold-launch
cost), then a fixed timed budget driven in 500k-step batches (each launch stays
well under the WDDM TDR window).

## Correctness signatures

Two FNV-1a hashes gate every A/B run:

- **`statehash`** — over guest RAM `[0, 0xA00000)` (code + heap + stack; the
  constant WAD region above `0xA00000` is excluded). This reflects **actual guest
  computation**, so it is the **real bit-exact regression check**: any change that
  alters emulated behavior changes this hash. Read back via
  `CudaEmulator.ReadRam`.
- **`fbhash`** — over the framebuffer (RGB channels). The framebuffer is **black**
  during cold-init, so this only confirms "still black" — a weak signal, kept for
  continuity. **Do not** rely on it to catch regressions; trust `statehash`.

## How to run

Build the native DLL first, then the project:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Native\rv32i_cuda.vcxproj -p:Configuration=Release -p:Platform=x64
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Examples_CUDA\Benchmark\Benchmark.csproj -t:Restore -p:Configuration=Release -p:Platform=x64
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Examples_CUDA\Benchmark\Benchmark.csproj -p:Configuration=Release -p:Platform=x64
```

Then:

```powershell
# Default: 20,000,000 steps (~4.6 s, lands mid-window, comfortably inside 2–8 s).
dotnet Examples_CUDA\Benchmark\bin\x64\Release\net10.0\Benchmark.dll

# Custom step budget (positional, must be > 0):
dotnet Examples_CUDA\Benchmark\bin\x64\Release\net10.0\Benchmark.dll 12000000

# Machine-parseable single-line CSV (for scripted A/B diffing). The flag may be
# combined with a step count in any order:
dotnet Examples_CUDA\Benchmark\bin\x64\Release\net10.0\Benchmark.dll --csv
```

Human-readable line:

```
benchmark: 20M steps in 4.78s = 4.18 MIPS   fbhash=F36CBE2231CE2B83  statehash=712E723EE40A46B8  halted=False
```

CSV line (`steps,seconds,mips,fbhash,statehash`, always `.`-decimal / invariant
culture so it parses on any locale):

```
20000000,4.7769,4.1869,F36CBE2231CE2B83,712E723EE40A46B8
```

## Baseline (single GPU thread, sm_86)

| Budget | MIPS | fbhash             | statehash          |
|--------|------|--------------------|--------------------|
| 20M    | ~4.30 | `F36CBE2231CE2B83` | `712E723EE40A46B8` |
| 12M    | ~4.27 | `F36CBE2231CE2B83` | `763DB1A440A711AF` |

MIPS varies a little with host/GPU thermal state (~4.0–4.3 observed); the hashes
are **invariant** and must not change for a perf-only refactor. The `statehash`
is budget-dependent (different step counts reach different guest state), so always
compare A/B at the **same** step budget. The default 20M baseline statehash is
**`712E723EE40A46B8`** — verified identical across repeated runs (determinism is
the whole point of this harness).

## How to use it for a perf change

1. Run `--csv` on the base, note the `statehash`.
2. Make your change, rebuild, run `--csv` again at the same budget.
3. The `statehash` (and `fbhash`) **must be identical** — if they differ, the
   change altered emulated behavior and is a correctness regression, not a perf
   win. Compare the `mips` field for the speedup.
