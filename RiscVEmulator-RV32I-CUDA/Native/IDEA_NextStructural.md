# Next structural mechanisms (profile-grounded, post-batch @ 2bcfc64)

Fresh HOTHIST profile of the MERGED build on the ttf30 workload (18.2M uop-execs), plus an
RVX_MAXW=8000 probe. Two hard facts drive everything below:

1. **~85% of remaining execs are plain 1:1 dispatched uops** — ALUI 32.1%, BR 17.6%, ALUR 15.9%,
   LOAD 11.3%, SUB 5.7%, ST 2.9% — scattered straight-line game/setup code, NOT fusable loops.
   The render loops are already collapsed (PALEXP/TEXSPAN/TEXCOL barely register). The cost per
   1:1 uop is dispatch (ladder walk + 3 descriptor `__ldg`s) + shared-RF round-trips, not ALU.
2. **The exec_block hybrid is wedged between two walls (measured):** at RVX_MAXW=24000 it sits on
   the ptxas memory knee (baseline 1.38 GB; ANY PTX perturbation — even mailbox's 3 instructions —
   explodes >2.6 GB, which blocked 6 written-and-gated batch units, archived as `archive/batch-*`);
   at RVX_MAXW=8000 the gate passes but coverage collapses and handoff thrash makes Doom **20×
   slower** (0.69 MIPS, 117 s ttf30). Neither knob fixes it — the architecture needs MULTIMOD
   and/or cheap handoffs first.

Leftover hot loops the fusion playbook still owns (exact, from hot.csv):
- `0x17170` lump-name scan (`lw; bne →exit; INCBR` 4 uops/iter) — **~15% of ALL execs** (676k iters).
- `0x0FBB8` word-fill (`STOREPI(sw,+4); bne` 2 uops/iter, 256k) — a `sw`-based memset; the MEMSET
  arm only matches byte fill.
- `0x113B8` screen-melt column (`LOAD; ALUI; STOREPI(sh,+320); ALUI; bne` 5 uops/iter, 248k) — the
  lh/sh strided copy with an interleaved ALU that COPYLOOPS' matcher rejects.
- `0x05F44+` a straight-line run of ≥6 consecutive ALUI ×180k — macroblock-shaped, no loop at all.

## Ranked mechanisms

**Tier 1 — cheap fused-arm extensions for the measured loops (days, same playbook):**
1. `WORDFILL` — sw-based fill loop → 1 native loop uop (word stores; the profile's 0x0FBB8).
2. `WORDSCAN` — the search-loop class `lw t,off(p); bne t,k →exit; p+=K; bne p,end` → 1 native scan
   uop. Previously dismissed as memory-bound-neutral, but it is 4 *dispatched* uops per 8 scanned
   bytes and 15% of all execs; with NC1 flat loads the native scan is tight. Measure it now.
3. `MELTCOL` — widen the strided-copy matcher to tolerate one interleaved independent ALUI and
   half-word width (the 0x113B8 shape): 5 uops/iter → 1.

**Tier 2 — attack the 85% 1:1 dispatch cost (the real lever now):**
4. **Two-level dispatch**: tier-1 ladder = ALUI/BR/ALUR/LOAD (77% of execs) behind ≤2 compares;
   ALL other classes behind one "rare?" gate. The ladder has grown to ~30 arms; the hot four
   currently walk past a fraction of them. Cheap, no BRX, measurable in a day.
5. **MACROBLOCK / BLOCKRUN uop** (roadmap #8, now with direct evidence — the 6×ALUI run at
   0x05F44): translator packs a straight-line 1:1 run into ONE uop executing a side-table micro-op
   list — one dispatch + sequential fetch instead of N ladder walks; full flavor also keeps
   intermediates in registers (cuts shared-RF round-trips to live-ins/live-outs). Risks known
   (ptxas local-spill on the slot array; the runtime-managed instruction-window precedent regressed
   — this is translator-baked, different); bounded prototype ≤6 ops, measure hit-rate first.

**Tier 3 — the structural jumps (weeks, biggest ceilings):**
6. **exec_block MULTIMOD** — split codegen into K functions of M words (e.g. 4×8000, one module or
   several): escapes the single-function ptxas knee at full coverage; cross-function transfers via
   XSAVE+redispatch (rare at region edges). THE unblock for the 6 archived xblk units (which then
   also re-land: cgmem/cgctl/loopemit each *shrink* per-instr PTX). Register-resident execution of
   the 85% is where the 2–5× lives; nothing else touches it.
7. **Cheap handoffs** (pinned mailbox + persistent kernel or CUDA-graph conditional loop) — the
   complement to #6; also what xblk-handback (archived, −80% as a launch storm) needs to pay off.
8. **WARPSPAN warp-cooperative render** — unchanged ceiling-raiser for the fused loops, and NOW
   CHEAPER: nc1flat means lane k's byte store at D+k is naturally coalesced flat memory (the
   interleave objection is gone for ncores==1). Pixel i = closed form in i, no loop-carried dep.

**Sequence:** 1→2→3 (cover ~25% of execs at low risk), 4 (cheap), 5 (prototype + hit-rate), then
commit to 6+7 as one program (they unlock each other AND re-land ~6 archived units), with 8 as the
parallel render-side ceiling break. Validation discipline unchanged: bit-identical gate, exact
weights, deterministic `--ttf`, pixel-exact shots.

## MEASURED OUTCOMES (branch `structural`, same session)

| # | Mechanism | ttf30 best | Verdict |
|---|---|---:|---|
| — | baseline (post-batch 2bcfc64) | 14.08 | reference |
| 1 | WORDFILL | 14.05 | KEPT (sub-noise solo, ~1.4% of execs; correct loop collapse, tail arm) |
| 2 | WORDSCAN | 14.47 | **KEPT (+2.8%)** |
| 3 | MELTCOL | 14.96 | **KEPT (+3.4%)** |
| 4 | dispatch ladder reorder (BR 2nd, SUB 5th, INCBR 7th) | **16.30** | **KEPT (+8.9% — the round's biggest win)**; the two-level gate variant is subsumed (hot 4 classes ≤4 compares) |
| 5 | MACROBLOCK/BLOCKRUN (overlay super-uop) | 15.71 | **REVERTED (−3.6%)** — after the reorder, 1:1 ALUI dispatch costs ONE compare, so the per-member ext loads cost more than the saved ladder walks; the "per-uop machinery regresses" lesson re-confirmed |
| 6–8 | MULTIMOD / cheap handoffs / WARPSPAN | — | multi-session builds, de-risked by this session's probes (maxw8k: low coverage thrashes 20×; knee: any PTX perturbation explodes at 24k → the .func-in-one-module design is the only viable MULTIMOD) |

Net this round: ttf30 14.08 → 16.30 (+15.8%); ttf90 15.61 → 16.60; aggregate diverge ratio improved
0.773 → 0.804 (BR 2nd helps the branchy guest), compute 3.42 → 3.58, data within noise. Gate
bit-identical throughout; f1 = 60,589 px pixel-exact; f40 clean.

## MEASURED OUTCOMES (50/75-MIPS iteration loop, post-MULTIMOD, baseline ~29.0 same-session)

| # | Idea | ttf30 (interleaved A/B) | Verdict |
|---|---|---:|---|
| i1 | cgmem port: cross-instruction address CSE + mod-4 alignment lattice in rvx_emit | 28.9 vs 28.9 | **REVERTED (neutral)** — root cause measured: 99,562 of 108,645 compiled words are dispatch entries (brx targets), so the fall-through state resets every ~1.1 instructions; PTX dump: 0 `[%ab]` reuses, ~125 proven-aligned ops of 146k. Sparsifying entries requires interpreter-handback machinery (archived neutral, thrash-risky). Dead-end as long as dispatch entries ≈ all leaders. |
| i2 | XS spill block .global → dynamic .shared (region transitions + entry/exit spill 62 ld/st) | 29.26 vs 28.97 | **KEPT (+1.0%)** — wins or ties every interleaved pair; `.extern .shared .b8 XS[]` in every unit aliases the 168 B dynamic-shared segment, launch passes 168 B. |
| i3 | SPARSE dispatch entries (99,562 → 14,932) + interpreter self-stop at entry pcs (uw bit7, xstop arg) + re-landed i1 CSE/alignment codegen | 29.84 vs 29.39 | **KEPT (+1.5%)** — wins all 4 interleaved pairs. Entries = branch/jal targets + return sites + post-uncompilable resume only; the interpreter exits at the first entry pc so sparseness can't strand chunks. Fall-through runs are now long enough that the i1 dead-end fires: 4795 base-cache seeds, ~13k proven-aligned memops, PTX −9%, 148 regs 0 B spill. Unlocks per-run budget-count decimation next. |
| i4 | Budget-count decimation (one add per atomic segment instead of per instruction) | 29.99 vs 29.98 | **REVERTED (tie)** — the adds hide entirely under memory latency; "fewer instructions ≠ faster when latency is already hidden" holds for exec codegen too. |
| i5 | Host-I/O flat fast path: ncores==1 read/write_mem = ONE plain cudaMemcpy instead of cudaMemcpy2D with 4-byte rows | **62.4 vs 30.0 (+108%)** | **KEPT** — RVX_STATS profile showed exec runs 100% of ttf30 (403 launches, 0 interp) at ~87 MIPS kernel-side; 58% of wall time was the harness's per-batch 256 KB framebuffer read going through a 64,000-row strided 2D copy. ncu: kernel stalls 36% memory scoreboard / 35% ALU dep, DRAM 2% — latency-bound, not bandwidth. |
| i6 | g_state + g_ret: cudaMallocManaged → PINNED zero-copy (cudaHostAlloc mapped) | **97.6 vs 62.3 (+57%)** | **KEPT** — on Windows/WDDM, managed memory migrates wholesale at every launch/sync boundary (~0.5 ms per StepN). Kernels touch CoreState only at launch entry/exit (regs live in shared/PTX registers), so PCIe scalar access costs ~µs while host reads become free. step_all non-GPU time: 225 ms → 0; GPU time itself also dropped (migrations were stalling launches). **Both goals passed: 50 ✓ and 75 ✓ (ttf30 ≈ 97-98 MIPS).** |
| i7 | exec_block multi-core: one thread per guest core, word-interleaved addressing baked into the PTX, per-thread XS slots in dynamic shared, per-core %S/%M' in the dispatcher, core-0-only RET | warp 730→16,798 (23×); block-scale: 256 cores 5.9k→93.1k, diverge 7.3× at all counts | **KEPT** — new [multicore verify] gate in CudaBench (halting per-core-seeded guest with misaligned traffic, full scratch-RAM hash per core) passes at 32 and 64 cores. Tradeoff: lockstep compute regresses at ≥16k cores (exec's 94–116 regs vs interp's 48 → occupancy at saturation; 47.6k vs 63.0k at 16k) while diverge wins everywhere (29.6k vs 12.6k at 16k); exec stays default-ON, RVX_OFF escapes. Driver: multi-core falls through to the interpreter after each exec launch so cores at non-enterable pcs always progress. Also fixed: the 5 fuzzer/test launches had passed 0 dynamic-shared bytes since XS moved to shared. |
| i8 | GLOBAL sp-alignment proof: if every reachable writer of x2 preserves 4-alignment (addi sp,sp,4k / lui / auipc / aligned andi; invalid encodings + 0x73 halt and write nothing), the lattice seeds sp ALIGNED at every reset → stack spills/reloads emit bare ld/st | Doom 98.6 vs 97.2 (+1.5%, all 4 pairs); CudaMath mandel 54→129 (2.4×), matmul 47→106 (2.2×), mandel warp32 620→4,034 (6.5×); PTX −17% | **KEPT** — stack traffic was the largest class of unproven memops (14-instr checked fallback each). Safety valves: set_reg(x2, unaligned) and translate-on-miss finding a bad writer disable exec. sp-proof status on the [xblk] built line. |

## 200-MIPS loop (baseline 97.3 with old harness; harness fixed at i-h1)
| # | Idea | Result | Verdict |
|---|---|---:|---|
| r1 | Shadow return stack (calls push (ra, baked pc2idx token) to per-thread shared stack; ret validates + re-enters via region brx, skipping XDISP's ~200-cyc global load) | Doom 97.3 vs 96.3 (+1.0%); mandel +6.4% | KEPT |
| h1 | ttf harness: 8-byte-wide frame detection (was byte-wise FNV ≈ 0.4 ms/batch ≈ 20% of wall) | 97.3 → 105.2 printed, GPU unchanged | KEPT — measurement overhead, guest work identical |
| h2 | Pinned staging bounce for bulk host transfers | 105 vs 105 | REVERTED (tie) — FB copy was already cheap; the gap is WDDM submissions |
| h3 | Staged-MMIO mailbox: pinned page + on-stream stage/drain micro-kernels replace ~12 sync memcpys/StepN | 105.2 → **110.7–115.5** | KEPT — host gap 132 → ~68 ms; subagent's top recommendation. CUDA-graphics interop evaluated and DROPPED (zero benchmark gain, SDL2 has no public D3D texture access). |
| r2 | Budget-count batching, retried at 122 MIPS kernel | 112.0 vs 111.8 | REVERTED (tie again) — %cnt adds stay hidden even now |
| r3 | Branch-free funnel lw (2 overlapping aligned loads + shf.r, +8 B buffer guard) for alignment-unproven flat-layout loads | **120.5 vs 112.2 (+7.4%)** | KEPT — wins all pairs; replaces the ~14-instr predicated checked path with 9 branch-free instrs, loads issue independently |
| r4 | Funnel load extended to lh/lhu | 112.5 vs 109.3 | KEPT (+~3%, all pairs) |
| r5 | Pipelined FB fetch: D2D snapshot (FIFO-safe on stream 0) + event-gated async D2H to pinned double buffer overlapping the next kernel; host consumes one batch late | **~119-125** | KEPT — the reshaped, race-free version of the user's stream/double-buffer idea; also removes the SDL live-buffer tear |
| r6 | Branch-over-fallback stores: unproven sw/sh aligned path = and/setp/bra-not-taken/st (4 issues); byte fallback moved to a COLD block after the region body (predicated-off instrs still consume issue slots — the old form charged ~8 squashed issues per store) | 122.3 vs 120.6 | KEPT (+1.4%, 4/5 pairs, tighter variance). ncu re-profile: 3.7 SASS/guest-instr @ ~4.4 cyc; stalls ≈ even split scoreboard/ALU-dep |
| r7 | If-conversion of short forward diamonds (1-2 ALU/const instrs → inverted setp + predicated body, predicated retire counts) | ~121.9 vs HEAD-equivalent ~123.9 | **REVERTED (neutral-to-negative)** — clang -O3 already emits branchless sequences for short diamonds on rv32i, so almost no sites match; the eligibility constraints (non-targ interior) kill the rest. Gates all passed; not a correctness issue. |
| r8 | lui/auipc+addi constant-pair folding (one mov, baked 32-bit constant) | 122.0 vs 122.6 | **REVERTED (−0.5%)** — ptxas -O3 already constant-folds dependent mov chains within a region function. LESSON: per-instruction PTX golf is exhausted (r7, r8 both neutral); remaining levers are things ptxas CANNOT see — semantic proofs (done: sp/funnel), dispatch structure (done: sparse entries/shadow stack), guest-memory latency (L2 persistence next), and the ~60 ms host gap. |
| r9 | L2 persistence window over low guest RAM (3.4 MB carve; tried 64 MB window @ hitRatio 0.05 and 8 MB @ 0.42) | tie / slightly worse | **REVERTED** — same conclusion as the interpreter-era L2 experiment: the single warp's hot lines already live in L1 (128 KB/SM, one resident thread) and the miss traffic is too scattered for a persisting carve to catch. L2 levers are now a confirmed dead end for this workload. |
| r10 | Region size sweep (zero-code env experiment, then default change): RVX_REGW 6000→60000 (Doom: 19→2 regions). Monotonic: 6k 121.5 / 12k 126 / 20k 129 / 40k 133 / 60k **139.6** (every interleaved pair). 1 region (15k brx targets) blew a ptxas child past 3 GB (watchdog-killed) → new default 60000 + a region-count floor keeping ≤8k brx targets per region (the knee scales with TARGETS, not words). | **121.5 → ~140** | **KEPT (+15%)** — cross-region transitions (31-reg spill/reload + dispatcher round trip) were the largest remaining structural cost. nc>1 keeps the 4000 clamp. |
| r11 | PALEXP whole-loop inline emission (exec-side fused palette-expand: 1 word store/pixel, hoisted invariants, exact temp write-back) | BLOCKED by the ptxas knee | **REVERTED** — gates all passed, but ONE instance of the new loop shape inside a 54k-word region tipped ptxas into a 3.9 GB / >300 s spin (the documented "+3 instructions explodes" knife edge: r10's big regions sit right on it). Re-attempt needs either small-region coexistence (palexp win must beat the ~10% region-size loss) or a knee-safe emission shape. |
| r12 | HARDENING (kept regardless): ptxas child watchdog (2.5 GB / 120 s, kill → fail) + automatic region-size halving retry in rvxblk_build + the in-driver PTX fallback now refuses units >256 KB (in-process knee, 13 GB commits historically). Time/RAM limits are now IN THE PRODUCT, not just the bench wrapper. | ttf30 unchanged (139-140) | **KEPT** |
| r13 | PALEXP whole-loop emission, retry with the fast loop in the COLD section (knee-dodging shape) | 104-112 vs 139-141 | **REVERTED — twice-confirmed dead end via two failure modes:** (a) even the cold-section shape trips the ptxas knee at 60k regions (watchdog killed a 2.75 GB child; the new retry landed the build at 30k automatically — hardening verified live); (b) at 30k regions the net is far BELOW even plain-30k (~131): the emission is net-negative at runtime, not just paying the region tax. Do not re-attempt whole-loop emission inside big regions; if ever revisited, it needs its own tiny dedicated .func unit (knee-isolated) and runtime engagement counters first. |
| r14 | RVX_PROF guest-pc sampler (slice exec budgets, histogram exit pcs, map via llvm-nm): ttf30 boot path = __umodsi3/__divsi3/__muldi3 ~27%, blob past _start 24%, vfs/WAD code ~19% — render loops are NOT the ttf bottleneck | diagnostic (kept) | Profiler is RVX_PROF-gated, zero cost off |
| r15 | Software-divide loop → ONE hw div.u32+rem.u32 (straight-line, knee-light; runtime precondition R==0∧Q==0∧i==31 routes mid-loop re-entries to the kept original; Q=N/D R=N%D i=-1, 416 retired atomically — div/rem-by-zero semantics match the soft loop exactly) | **144.0 vs 137.6 (+4.6%, all pairs)** | **KEPT** — even though the addition tips the 60k knee and the build lands at 30k regions, the divide win exceeds the region tax. Plus: persisted knee-discovery marker (%TEMP%\rvcud_regw_<imghash>) — without it every process start replayed the ~10 s failed 60k attempt and the idle stall de-boosted GPU clocks (the fake ~25% slowdown that poisoned r13's A/B too: palexp@30k was never fairly measured, but its r11 inline form remains dead). |
| r16 | Software-multiply loop → ONE mul.lo (straight-line, valid from any entry state, clz-derived exact retire) | mul@15k 132.9 vs div-only@15k 127.6 (+4%) BUT div-only@30k = 144 | **REVERTED (net −11)** — the replacement itself wins at equal region size, but its ~500 FixedMul sites push ptxas past the 120 s child watchdog at 30k regions → auto-landing at 15k, and the region tax exceeds the multiply win. Re-viable only if the knee instability is solved (e.g., per-site emission budget, or longer child timeout traded against startup). Knee markers are keyed on image+default — STALE MARKERS MUST BE DELETED when reverting codegen (a leftover 15k marker pinned HEAD's A/B runs to 15k and masqueraded as a regression). |
