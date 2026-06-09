# Roadmap: pushing single-thread Doom past 10 RV32I MIPS

## The wall (measured this session)
Single Doom = **one GPU thread at max clock (1950 MHz), ~5M uops/s, ~400 cycles/uop**. Base and rvcud
have *identical* uop throughput — fusion (ratio 1.9) is the only thing that took us 4.9 → ~10 MIPS by
doing more guest-work per uop. The per-uop floor is **guest-memory-access latency**: Doom's renderer is
texture reads + framebuffer writes to `g_mem` (global, L2 ~200 cyc / DRAM ~400 cyc), and on **one lane
there is nothing to overlap that latency with**. Everything that doesn't attack that latency regressed
(register forwarding −5%, forced reg count −7%, L1 carveout neutral).

So there are exactly four ways forward, and the big wins are in the first two:

| Direction | What it attacks |
|---|---|
| **A. Hide the latency** (async copy / prefetch / ILP) | the un-overlapped ~400 cyc |
| **B. Escape the single lane** (warp-cooperative SIMT) | the "nothing else to do" |
| C. Reduce memory ops (cache hot data, eliminate loads) | fewer ~400-cyc stalls |
| D. Reduce uops (deeper fusion, in-register intermediates) | the multiplier on every stall |

The **decisive enabler** for almost all of this is the thing you pointed at: **we have the entire program
statically** — every uop, jump, load, and its data-flow — *before* execution. That lets the translator
become a real ahead-of-time optimizing compiler (dependence graph → prefetch scheduling, load forwarding,
register promotion, LICM, macro-op formation), instead of the peephole-fusion it is today.

---

## Tier 1 — the big levers

### 1. Static-scheduled async prefetch (`cp.async`/LDGSTS) into a shared scratchpad — *the preload mechanism*
**Mechanism.** The renderer's hot loops stream memory (column blit reads a texture column; span blit
reads a row). At translate time, for each load we compute — from the dependence graph — the *future*
load(s) the loop will need (address = current base + stride, known statically) and how many uops of slack
exist until the value is consumed. We emit a **`PREFETCH`/`cp.async`** that streams that future data
global→shared **asynchronously** (LDGSTS, truly async on sm_86, the thread keeps computing), double-
buffered: compute on stage A (in shared, ~30 cyc) while LDGSTS fills stage B. `cuda::pipeline<thread_
scope_thread>` + `__pipeline_commit`/`__pipeline_wait_prior(N)` (the low-level primitives guarantee
LDGSTS). 16-byte copies use L1-BYPASS (don't pollute L1); 4/8-byte stay L1-cached.
**Why it wins.** Directly converts the ~400-cyc un-hidden load into a ~30-cyc shared read, with the
global latency overlapped behind compute. This is the single most direct attack on the measured
bottleneck, and Doom's blits are textbook streaming producer-consumer.
**Static-analysis leverage:** look-ahead address computation + optimal prefetch distance (#9).
**Impact:** ~1.5–3× on memory-bound render loops. **Effort:** medium-high. **Risk:** medium (correctness
of the sliding-window guest↔shared mapping; the gate + render check it).

### 2. Warp-cooperative SIMT for the data-parallel inner loops (the renderer)
**Mechanism.** Today 1 thread = 1 core. Add a mode where a **warp (32 lanes) cooperates on one Doom**:
the serial interpreter runs on lane 0, but when it reaches a *statically-recognized data-parallel loop*
(column/span blit, memcpy/memset, R_DrawColumn's per-pixel loop), it broadcasts the loop parameters
(shuffle) and the **32 lanes process 32 elements in parallel**, then re-converge. The COPYLOOP that today
word-widens 4× becomes 32× (one element per lane); contiguous accesses *coalesce* (one 128-byte
transaction for the warp instead of 32 serial loads).
**Why it wins.** Breaks the "one lane has nothing to overlap with" root cause for the parallelizable hot
loops — and the renderer's pixel loops are embarrassingly parallel. The serial fraction stays on lane 0
(Amdahl: if ~60% of time is parallelizable and goes ~32×, overall ≈ 2.4×).
**Static-analysis leverage:** identify the data-parallel loops (no loop-carried dependence except the
induction pointers) from the dependence graph; emit a `WARP_BLIT`/`WARP_LOOP` uop.
**Impact:** ~2–2.5× (Amdahl-bounded). **Effort:** high (warp coordination, lane-0 regfile broadcast).
**Risk:** high — but transformative; it's the only idea that raises the *per-uop* ceiling rather than
hiding latency. Note: trades aggregate throughput (32 lanes = 1 Doom vs 32 Dooms) for single-Doom speed —
correct for the single-instance goal.

### 3. Static memory-region classification → framebuffer in shared, read-only data via `__ldg`/texture cache
**Mechanism.** Data-flow-trace each load/store's base register to its origin. Stores whose base provably
derives from the **framebuffer pointer** → mark `ST_FB`, routed to a **shared framebuffer scratchpad**
(64 KB fits; single-core uses ~no shared today) with **no runtime range-check** (the routing is baked
into the uop class — this is why it avoids the per-op-check cost that sank register-forwarding). Flush
shared→`g_mem` at the frame boundary (the present-MMIO store). Loads from the **read-only** WAD/texture
region → mark `LOAD_RO`, use `__ldg` (read-only data cache) or a bound CUDA **texture object** (2D
locality + hardware cache) for texel fetches.
**Why it wins.** The renderer writes the framebuffer constantly; shared (~30 cyc) vs L2 (~200 cyc) is a
~6× cut on the write side, and `__ldg`/texture caching improves the read side — both with *zero* per-op
overhead because classification is static.
**Static-analysis leverage:** region inference via base-pointer data-flow. **Impact:** ~1.3–1.6×.
**Effort:** medium. **Risk:** medium (region inference soundness; conservative fallback to normal `g_mem`).

---

## Tier 2 — turn the translator into an optimizing compiler (compounding, all leverage the static view)

### 4. Proper SSA-ish IR + dependence graph (the foundation)
Build per-function basic blocks with use/def, a dependence DAG, and alias info. This *enables* #1, #5–#10.
Today's translator is peephole; this is the structural investment that unlocks the rest.

### 5. Static load-forwarding & redundant-load elimination
If the same address is loaded twice with no aliasing store between (provable from the IR), forward the
first value → delete the second memory op (a register copy). Doom reloads struct fields constantly.
**Impact:** removes ~400-cyc stalls outright. **Effort:** medium (needs alias analysis).

### 6. Register promotion / scalar replacement of hot memory locations
A guest memory location accessed repeatedly in an alias-free region (e.g., the screen pointer, loop
accumulators) → load once into a CUDA register, keep it there, store back at region exit. Classic scalar
replacement. **Impact:** turns repeated memory traffic into register ops. **Effort:** medium-high.

### 7. Loop-invariant code motion
Hoist address-base / invariant computations out of hot loop bodies into a preheader uop → fewer uops and
fewer loads per iteration. **Impact:** small-medium, cheap once #4 exists.

### 8. Macro-op / superblock fusion with in-register (virtual) intermediates
Generalize LEA/XSH/MULADD: fuse a straight-line run of dependent ALU ops into one **macro-uop** that runs
a tiny micro-op list **entirely in CUDA registers**, reading only live-ins and writing only live-outs of
the *block* (not per op). This is the real attack on shared-regfile traffic — and on the texture-
coordinate bit-math (`srli/slli/and`) that point-fusions couldn't fold. **Impact:** cuts shared-RF ops
from per-op to per-block. **Effort:** medium-high (encode a micro-op list; bounded length, e.g. ≤4).

### 9. Optimal prefetch-distance scheduling
For each load, compute slack (uops until consumption) from the DAG and schedule its prefetch (#1) exactly
that far ahead — neither too early (evicted) nor too late (not ready). The whole-program view makes this
exact rather than heuristic.

### 10. Trace scheduling / hot-path block layout
Order the uop stream so the hot path is fall-through (loop bodies contiguous, likely-taken branches not
taken) → tighter instruction fetch and fewer taken-branch uops. Use loop back-edges + the execution
histogram as the profile.

---

## Tier 3 — more fusions & domain-specific (histogram-driven, lower individual EV)

### 11. Histogram-driven point fusions
Re-profile the *current* hot path and add the top 2/3-op pairs only if they fire often enough to beat the
~3%/dispatch-arm cost: scaled-indexed `LDXS` (`slli;add;load`), bitfield-extract (`srli;andi`),
`and;add`, etc.

### 12. Doom-specific `TEXSAMPLE` uop
Recognize R_DrawColumn's inner idiom (fixed-point texture-coordinate step + wrap-mask + texel fetch) and
fuse the whole per-pixel body into one uop. High value (it's the renderer core) but Doom-specific.

### 13. Texture/surface memory for texel reads (overlaps #3)
Bind the WAD texture region to a CUDA texture object so texel fetches use the texture cache (built for 2D
spatial locality) and dedicated path — independent of the L1/L2 data path.

### 14. M-extension recompile (`rv32im`) — the FPS lever, *not* the MIPS-metric lever
Hardware `mul`/`div` collapse the software loops (we measured FPS 1.04→1.53). But it **lowers the RV32I
instruction count** (1 `mul` vs ~100), so it *reduces* the RV32I-MIPS metric while making Doom render
faster. Keep `RC_MEXT` (already implemented, dormant) for when the metric is wall-clock/FPS rather than
RV32I-MIPS. Worth a dedicated FPS-oriented build.

---

## Tier 4 — moonshots (biggest ceiling, biggest effort)

### 15. True JIT: translate hot basic blocks to PTX, NVRTC-compile, launch native
Eliminate interpretation entirely for the hottest blocks: the host translator emits PTX for a hot
basic-block trace, NVRTC/driver-API compiles it once, caches the module, and the driver loop launches the
native block instead of interpreting uops. No per-uop decode/dispatch/regfile-shuffle at all — the block
runs as compiled GPU code with registers in registers. This is the logical endpoint of "transcompile."
**Impact:** potentially 2–5× (removes the entire interpretation tax). **Effort:** very high (PTX codegen,
compile-latency management, module cache, guest-state ABI). **Risk:** high. The right *eventual* target.

### 16. Warp-as-helper-threads (latency hiding without parallelizing the guest)
Lane 0 interprets; lanes 1–31 are **helper threads** that, using the static load schedule, pre-touch
(prefetch) the global addresses lane 0 will need N uops ahead, warming L1/L2. Unlike #2 it doesn't
parallelize the guest — it just hides latency with idle lanes. Simpler than #2, complementary to #1.

---

## Suggested sequence (EV-ordered, each gate-validated + render-checked)
1. **#4 dependence-graph IR** — foundation; unlocks the rest. Build it first.
2. **#1 async prefetch into shared** (+ #9 scheduling) — most direct hit on the bottleneck, keeps the
   1-thread model. Prove on COPYLOOP/blits first.
3. **#3 framebuffer-in-shared (static routing)** + **#5 load-forwarding** — cheap once #4 exists, cut the
   write/redundant-load latency.
4. **#8 macro-op fusion** + **#6 register promotion** + **#10 layout** — compound the per-uop reduction.
5. **#2 warp-SIMT blits** — the big structural jump for single-Doom; do after the above so the serial
   fraction is already lean (better Amdahl).
6. **#15 JIT-to-PTX** — long-term ceiling raise.

Validation for every step is unchanged and non-negotiable: CudaBench `rvcud bit-identical ✓` gate
(exact retired-count), Doom renders pixel-perfect, CPU-validate any loop-collapsing transform
(as with COPYLOOP/MULLOOP), and read the `RV32I MIPS = uop-throughput × fusion-ratio` line with the FPS
cross-check (they must move together — exposes any inflated weight).

## Honest expectation
No single idea is a clean 10→20. But #1+#3 (hide + shorten memory latency) plausibly compound to ~1.5–2×,
#8/#5/#6 add ~1.2–1.5×, and #2 (warp-SIMT) is the ~2× structural jump — together a realistic path to
**~20–30 RV32I MIPS** single-thread, with #15 (JIT) as the ceiling beyond that. The throughline is your
insight: **use the whole-program static view to schedule memory ahead of time and keep transient values
off the shared regfile** — that's where the cycles are.

---

## Implemented as `#ifdef` feature flags (default OFF — flip on and measure)
Written into `Native/rv32i_cuda.cu` behind compile-time flags so the committed-best path (099e7bb,
~10.7 MIPS) is **byte-for-byte unchanged** until a flag is set. Each is independent and each **preserves
the rvcud↔rv32i bit-identical gate** by construction (it changes cache policy / memory staging / uop
count, never the architectural result). Compile-checked both ways (all-OFF and all-ON) with nvcc.

| Flag | Roadmap | What it does | Expectation / risk |
|---|---|---|---|
| `RVCUD_CPASYNC` | #1 | `cp.async`/LDGSTS-streams the **COPYLOOP** source through a shared double-buffer (16-B chunks; global read of chunk N+1 overlaps the store of chunk N). Single-Doom only (`ncores==1`, where guest RAM is linear so the region is contiguous & stage-able). Byte-identical to the scalar copy. | The direct attack on the bottleneck — *but* one warp has little to overlap with and the prior IV-prefetch attempt regressed, so **measure, don't assume**. |
| `RVCUD_STREAM_FB` | #3 | Stores to the framebuffer region (`addr ≥ FB_BASE = 0x20000000`) use `st.global.wt` (write-through, L1-bypass) so constant FB writes don't evict hot read data from L1. | Cheap, but adds one runtime address compare per rvcud store — the *same* per-op-overhead pattern that sank register-forwarding. Likely a wash or small win; measure. |
| `RVCUD_BITFIELD` | #8/#11 | Fuses `srli rt,rs,k ; andi rd,rt,mask` → one `BFE` uop (`rd = (rs>>k)&mask`); the shift temp never reaches the shared regfile. Targets Doom's texel/colormap bit-math. | Pure uop-count reduction, sound, no runtime cost beyond +1 dispatch arm (~3%). Wins iff the pattern fires often enough to beat the arm cost. |
| `RVCUD_LOADFWD` | #5 | Translator pass: a load matching an earlier same-block load (base unchanged, no store between, earlier dest still live) → register copy (`ADDC rd,rA,0`), dropping the memory op. Sound (mem provably unchanged); host-side only, zero runtime cost. | clang -O2 already elides most redundant loads, so EV is low — the flag MEASURES the Doom hit-rate. Foundation-free (#4) so it's intentionally conservative (intra-block, any-store kills). |

**Build a variant DLL** (from `Native/`, in a dev shell):
```
nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 -shared -cudart static \
     -DRVCUD_CPASYNC=1 -o rv32i_cuda.dll rv32i_cuda.cu        # add/remove -D flags to taste
```
Copy the DLL next to `Examples.CudaBench.exe` and `Examples.CudaDoom.exe`, then **validate in order**:
1. CudaBench → `rvcud bit-identical ✓` (non-negotiable; a flag that breaks the gate is a bug, not a perturbation).
2. `Examples.CudaDoom.exe --rvcud --bench` → read the `RV32I MIPS = …` line (best-of-N) and the FPS cross-check.
3. `Examples.CudaDoom.exe --rvcud` → eyeball one frame (pixel-perfect render).

Keep only the flags that beat 10.7 on the gate-clean bench; revert (leave OFF) the rest.

### Deliberately NOT done as flags (and why)
- **#2 warp-cooperative SIMT** and **#15 JIT-to-PTX (NVRTC)** are **structural rewrites**, not toggles —
  warp-SIMT changes the thread↔core mapping and the whole launch/regfile model; JIT-to-PTX adds a runtime
  codegen+compile+module-cache subsystem. Neither reduces to an `#ifdef` over the current kernel, and
  neither can be meaningfully validated without GPU time. They remain the big-ceiling items to design
  next, *after* the flags above are measured and the serial path is as lean as it can be.
- **#4 IR / #5 load-forwarding / #6 reg-promotion / #7 LICM**: the foundation (#4) is a real build-out;
  on already-`-O2`-optimized clang output #5/#6 fire rarely (clang already keeps live values in registers
  and elides redundant loads), so their EV is low until #4 exists and we can measure hit-rates. Not faked.
