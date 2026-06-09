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
