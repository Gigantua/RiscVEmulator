# NEXT — push CudaDoom ttf90 from ~226 to 250 MIPS (engine-side only)

State as of 2026-06-11 (late): Steps 1–3 below are DONE. r32 re-landed (`8d5e685`,
+2.7%), r33 PGO layout landed (`64707ed`, +1.2%), r34 reverted (−1.7%), r35
diagnostics closed Step 3 (see `Native/IDEA_NextStructural.md` r35 row).
Reference good state: **r33 = HEAD** — fresh build `5F6D6C6C` deployed to all 6
bins, all gates green. Evening window prints 196–210; morning projection ~240+.

## Ground rules (violations got reverted — do not relitigate)

1. **Never edit guest sources**: `Examples/Doom/Programs/PureDOOM.h` is forbidden.
   `doom_main.c` (pacing/MMIO glue) and `Runtime/runtime.c` count as guest-visible:
   only with an explicit user ask. All work happens in `Native/rv32i_cuda.cu`.
2. **Metric**: `Examples.CudaDoom.exe --rvcud --ttf 90` printed MIPS
   (steps = actually-retired via `cuda_rvcud_retired()`, frames = guest VSYNC presents).
   ttf30 ≠ ttf90 ≠ pre-h4 numbers. Only baseline-vs-change comparisons, never absolutes.
3. **Clock window**: identical bits printed 226–231 (quiet morning) and 184–196
   (evening: video player + 2× VS + Chrome on the GPU). Before believing any number:
   `nvidia-smi --query-gpu=temperature.gpu,clocks.sm,utilization.gpu --format=csv`
   → utilization should be ≲5 % with nothing running. Every A/B is interleaved
   base,cand,base,cand within the same minutes; 3rd pair on mixed signs.
4. **Hash-verify swaps**: print `(Get-FileHash …rv32i_cuda.dll).Hash` (or the ELF)
   before every A/B run — a locked file makes Copy-Item fail and silently re-measures
   the wrong side (it happened twice). Retry-loop all copies (20 × 500 ms).
5. **Gates whenever the DLL changes** (all must pass before any A/B):
   - `Examples.CudaBench.exe --rvcud` → both "bit-identical ✓" lines
   - `Examples.CudaMath.exe` → "all results bit-exact ✓"
   - `Examples.CudaBench.exe --fuzz` → 0 failures in all phases
6. **Every run goes through the guarded runner**:
   `& C:\work\RiscV\RiscVEmulator\.claude\grun.ps1 -exe <exe> -args "--rvcud --ttf 90" -match "time-to" -tmo 580`
7. Log every round (kept AND reverted) as a row in `Native/IDEA_NextStructural.md`.
   Read its dead-end rows before picking anything new.

## Build / deploy recipe

```powershell
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 -diag-suppress 549 -shared -cudart static -o `"<repo>\Native\bin\Release\rv32i_cuda.dll`" `"<repo>\Native\rv32i_cuda.cu`" -lcuda 2>&1"
# deploy to: Examples\{CudaBench,CudaMath,CudaDoom,CudaVoxel}\bin\x64\Release\net10.0
#        and Examples\CudaTinyCC\bin\{Release,x64\Release}\net10.0
```
First Doom run after any PTX-affecting change pays ~150 s/unit assembly once
(cubin disk cache `%TEMP%\rvcud_xblk_<hash^olvl>.cubin`, knee markers
`%TEMP%\rvcud_regw_<imghash>.txt`). Discard that run; measure warm only.

---

## ✅ GOAL MET — w1 warp-cooperative units (2026-06-11 late)

**284 MIPS, measured in a LOADED evening window** (base r33 printed 217 in the
same minutes) — the 250 goal is exceeded with margin; a quiet morning window
will read higher. See the `w1` row in `Native/IDEA_NextStructural.md`.

The path there: Step 3's ncu re-profile showed `wait` (single-warp ALU latency)
at 49% — the scalar core can't be parallelized. But the new `RVX_UNITPROF`
%clock accounting (env-gated, kept as tooling) showed **38.3% of all kernel
cycles run inside the fused helper units** (xpal 22.1%, xcopy 10.4%). w1:
ncores==1 launches xk with 32 lockstep-redundant threads (SIMT makes the 31
extra lanes free) and xpal/xcopy split iterations across lanes. +31% all pairs,
frame-80 byte-identical, all gates green, nc>1 PTX unchanged.

w2 landed the follow-up: cooperative xscan/xscpy → **~291 MIPS** (evening
window, +4.4% over w1, all pairs). Units now 5.99% of kernel cycles — the
fused-unit lever is EXHAUSTED. f80-capture rule learned: pixel compares need a
quiet GPU and matched pacing (content is demo-tic-dependent; see w2 row).

**h6 (user-directed): present gate 14000→1000 µs** — post-w2 the engine outran
the 70 Hz cap (wall pinned at the gate floor; gains showed only as spin steps).
New compute-bound baseline: **~500–596 MIPS ttf90** (steps bit-stable ~181 M).
The 310-MIPS goal is exceeded ~1.9×. Core scaling (CudaBench `--scale`):
compute 419/core @1 → 11.6 G agg @32; data 222→1685 agg; diverge 142→254 agg.
Left on the table (only if a new goal appears): the Tier-3 MULTIMOD
register-resident program for the scalar `wait` bucket (~50%), and the
cross-region direct-call idea (DLOOP round trips; measure with the new
UNITPROF DLOOP counter first).

Everything below is the COMPLETED plan, kept for the record.

---

## Step 1 — re-land r32: hot-path backward-branch budget checks  (~+2.7 %, proven) — DONE `8d5e685`

DLL-only, measured 232.1/231.0/231.4 vs 220.7/227.9/227.3 (all 3 pairs, gates green),
then reverted only as part of the blanket rollback. The exact change is commit
`f577f75`. Re-land with:

```powershell
git checkout f577f75 -- RiscVEmulator-RV32I-CUDA/Native/rv32i_cuda.cu   # cu was otherwise untouched between r27 and r32
git commit -m "reland r32: hot-path backward-branch budget checks (+2.7%)"
```

What it does (in `rvx_emit`'s `xfer` lambda, ~line 2555): backward in-region branches
stop paying `2× setp + 2 predicated-off slots + bra` per execution. New shape:
- conditional: `setp.lt.and.s32 %p1,%cnt,%budget,%p0; @%p1 bra L<t>; @%p0 bra XB<pc>;`
  with cold stub `XB<pc>: mov.u32 %pc,<t>; bra XSAVE;` appended to the existing
  `cold` string (it is emitted after the region body).
- unconditional: `setp.lt.s32 %p1,%cnt,%budget; @%p1 bra L<t>; mov.u32 %pc,<t>; bra XSAVE;`

Then: full gates, warm-up run, 3 interleaved pairs vs an r27-built baseline DLL
(keep a copy `rv32i_cuda_base_r27.dll` in the CudaDoom bin before deploying).

## Step 2 — r33: PGO hot/cold segment layout  (est. +2–4 %, design ready) — DONE `64707ed` (+1.2%)

Attacks the ncu-measured fetch stalls (`no_instruction` 0.42 of 5.93 cyc/issued;
`branch_resolving` 0.73): hot code is scattered through ~11 MB of SASS. Emit
sampled-hot segments first within each region; sink never-sampled segments to the
region tail. Design (was half-implemented, then discarded in the rollback — redo):

1. **FNV helper** (before `rvx_codegen`):
   `static unsigned long long rvx_fnv(const void* data, size_t bytes)` — FNV-1a,
   same constants as the knee-marker hash (1469598103934665603 / 1099511628211).
2. **Profile dump** — in `rvx_prof()`'s `atexit` lambda (~line 4498), before printing:
   write `%TEMP%\rvcud_prof_<rvx_fnv(g_img)>.txt` as
   line 1 `"<g_prof_base> <g_prof_hist.size()>"`, then one `"<bucketIdx> <count>"`
   per nonzero bucket (256-byte buckets). Print the path so runs are auditable.
3. **Loader** — top of `rvx_codegen` (after `body[]` is built): if
   `%TEMP%\rvcud_prof_<rvx_fnv(img, N*4)>.txt` exists, mark
   `hotw[w]=1` for every word in every nonzero bucket
   (`w0 = (pbase + idx*256 - base)/4`, 64 words per bucket); `havepgo = true`.
   `RVX_STATS=1` → print `[xblk] pgo layout: %d hot words`.
4. **Reorder** — replace the region emission loop
   (`for (; bi<nb && regof[body[bi]]==r; bi++)`) with:
   collect the region's words into `emitw[]`; split into segments at
   `emitw[i] != emitw[i-1]+1 || targ[emitw[i]]`; a segment is hot if any word has
   `hotw`; emit hot segments first (original relative order), then cold ones;
   iterate `for (size_t ei=0; ei<emitw.size(); ei++) { uint32_t w = emitw[ei]; … }`.
   The existing `(int)w != prev+1 || targ[w]` boundary check already resets
   cse/flushcnt at every displaced seam — keep `prev` tracking the guest address.
5. **Fall-through patch** — in the per-word epilogue where region-leaving
   fall-through already branches to XSAVE, add the displaced case:
   ```cpp
   else if (ei+1 >= emitw.size() || emitw[ei+1] != (uint32_t)nw) {
       flushcnt(0); rvx_app(ptx, "bra L%u;\n", base+(uint32_t)nw*4);
   }
   ```
   (every emitted word gets an `L%u:` label, so the target always exists).
6. **Workflow**: build → gates (no profile file exists for the gate guests →
   identity layout, gates unaffected) → generate the Doom profile with ONE
   `RVX_PROF=120 …--ttf 90` run → normal runs now pick the file up (new PTX hash →
   fresh cubins, ~150 s once) → 3 interleaved pairs vs r27 baseline DLL
   (baseline ignores the file).
7. **Risks**: ptxas knee is order-sensitive (r11 lesson) — the 2.5 GB/360 s watchdog
   and halving retry protect; if discovery degrades the region size below 30000,
   that's an automatic verdict against the idea. Stale profiles are keyed by image
   hash, so a guest rebuild orphans them (harmless: identity layout).

## Step 3 — smaller engine candidates, in order — DONE (r34/r35)

- **xscan header gate** (~line 3853): tried in r34, sub-noise, reverted with the
  bundle (lean-code rule). Dead.
- **XDISP/jalr cost**: bounded from the r35 stall table without instrumentation —
  < ~0.5 % of wall (inside long_scoreboard 4%, dominated by guest loads). Dead.
- **ncu re-profile after Steps 1–2** (r35): `no_instruction` did NOT drop (0.44
  vs 0.42) — r33's wall win stands but fetch stalls didn't move. Top bucket =
  `wait` 3.04/6.18 (49%) → only the Tier-3 structural program reaches it.

## Dead ends — never retry (full reasons in Native/IDEA_NextStructural.md)

60k regions · rv32im guest · inline whole-loop emission in big regions (r11/r13) ·
BRX dispatch · per-op runtime checks · L2 persistence · interpreter budget batching ·
%cnt add batching (r2) · diamond if-conversion (r7) · constant folding (r8) ·
pinned bounce · -O1/-O2 ptxas · **any PureDOOM.h/guest edit** (r28/r31/h5 — all
rolled back at user request; h5's 1 ms present gate also flooded the host with
256 KB framebuffer copies).

## Expected arithmetic

r27 quiet-window baseline ≈ 226–231. Step 1 ≈ ×1.027 → ~232–237.
Step 2 at +2–4 % → ~237–246. Remainder from Step 3 / a second ncu-guided round.
Verify the final number in a quiet window with 3 consecutive warm runs ≥ 250.
