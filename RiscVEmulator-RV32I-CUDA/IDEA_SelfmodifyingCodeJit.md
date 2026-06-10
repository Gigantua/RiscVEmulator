# PLAN: TinyCC (runtime-generated code) on the rvcud fast path

**Status:** planned, v2 (2026-06). Rewritten against the current architecture — the v1 note
predated exec_block, translate-on-miss, and the halt-pc work, all of which now exist. TinyCC
today runs correctly but ONLY on the base per-instruction kernel (`Examples/CudaTinyCC` never
sets `UseRvcud`); this plan is what it takes to run it on the rvcud uop core.

---

## 1. The three engines today, and what TinyCC needs

| engine | fetches from | covers | TinyCC status |
|---|---|---|---|
| **base** (`rv32i_kernel`) | live guest RAM, per instruction | anything, incl. self-modifying | **works today** (slow, ~1× ) |
| **rvcud** (uop interpreter) | pre-built uop stream over the static code span `[0, codeHi)` | static code + lazy 1:1 blocks via translate-on-miss | halts on JIT'd code (outside the map) |
| **exec_block** (PTX JIT) | per-region PTX compiled at `set_code` from the static image | statically-reached words only | N/A for runtime code (by design — see §6) |

TinyCC's guest flow: the statically-compiled TinyCC binary (≈400 KB, fully inside the
translated span — rvcud + exec handle it fine) compiles C source from guest RAM into a
**malloc'd buffer in the heap**, then `jalr`s into it. Two properties matter:

1. The generated code lives at heap addresses — **outside `g_pc2words`**, so not just
   untranslated but *unmappable*: `rvcud_translate_miss` bounds-checks the pc against the
   static window and bails. The JALR arm then can't even stop-at-target.
2. The demo compiles three programs; if TCC reuses/frees+reallocs the buffer, previously
   translated dynamic code is **overwritten** → stale uops (true SMC).

## 2. What v1 planned that is NOW BUILT (don't redo)

- **Translate-on-miss** (`rvcud_translate_miss`): JALR to an untranslated *in-window* word
  surfaces the pc (`resume_pc`), the driver translates a straight-line 1:1 run and resumes.
  This is what unblocked DOOM's function-pointer dispatch.
- **Miss/halt pc surfacing**: the interpreter epilogue now preserves the faulting pc next to
  `HALT_BIT` (commit b88a617); `cuda_rv32i_get_pc/get_reg` exist for the host.
- **Validation harness**: `cuda_rvcud_fuzz` (forward + jalr/backward phases) differentially
  tests the full translator+interpreter pipeline; `RVCUD_FUSEMASK` / `RVCUD_TRACE` exist for
  bisecting. Any work below gets a fuzz phase, not just an eyeball.

## 3. The known break, measured (voxel postmortem, 2026-06)

Translate-on-miss is **fragile by design** for loopy code: miss-translated runs bake direct
branch/jal targets against the *live* map; a target not yet translated bakes the
`0xFFFFFFFF` sentinel, and **taking it kills the core** (`ui = 0xFFFFFFFF` → OOB `uop2pc`
read → garbage pc / lost-halt). We hit exactly this when `ReadOnlyCodeSpan` truncation
pushed `draw_triangle` into miss-translated territory: the first taken backward branch died.
It is fine for the call-stub-sized blocks DOOM needed; it cannot host a JIT'd function with
loops. **Fixing this is the core of the plan** — everything else is bookkeeping.

## 4. Phase D1 — dynamic code window (rvcud runs TinyCC output, no SMC yet)

1. **Cover the heap in the pc map.** Extend `pc2uop` to a second window (or simply widen
   `g_pc2words` to all of guest RAM: 16 MB RAM → 4 M words → 16 MB device table + the same
   for `uop2pc` growth — acceptable; allocate the map full-RAM, lazy-zero). Words outside
   the static image start `RC_BADUOP` like any untranslated word.
2. **Growable uop buffers.** `g_uopcap = nuops + 2N + 16` is sized for static re-emission
   only. Dynamic guests need chunked growth: over-allocate (e.g. +64 K uops), and on
   exhaustion realloc device buffers between launches (host owns mirrors already:
   `g_w0h/g_w1h/g_uwh/g_u2pch`).
3. **Make unresolved-taken recoverable** (the §3 fix). Two pieces:
   - In the miss-translation, record a host-side **fixup list**: `(uop index, target word)`
     for every branch/jal baked as `0xFFFFFFFF`.
   - Kernel: a taken unresolved branch must exit with the TARGET pc surfaced, not poison
     `ui`. Encode unresolved as a dedicated sentinel the BR/JAL arms test (`w1 ==
     0xFFFFFFFF` → `resume_pc = <target pc>; break`). Target pc is computable: bake the
     *guest target pc* in a side table, or re-derive from `uop2pc[ui]` + the original
     instruction (host translates from there anyway).
   - Driver: on such an exit, translate the target block, then walk the fixup list and
     patch every recorded `w1` that now resolves (device patch = small `cudaMemcpy` into
     `g_uops`, same as translate_miss does today). Resume. Each block faults at most once
     per unresolved edge.
4. **Fusion off for dynamic blocks** initially (1:1 classify, exactly like translate_miss
   today). TCC's output is naive code — fusion would help — but correctness first; the
   FUSEMASK machinery makes turning matchers on per-window a later A/B.
5. **Driver loop placement:** all of this happens at the existing `cuda_rvcud_step_all`
   miss-handling point — no host API change, `CudaEmulator` untouched except a `UseRvcud`
   flag in the CudaTinyCC harness.

**Gate:** CudaTinyCC output (fib/primes/mandel text) byte-identical base vs rvcud, plus a
new fuzz phase: generate a random "stage-2" program into a heap address at runtime (host
pokes it via `write_mem` mid-run), jalr to it, compare against `rvx_ref_step`.

## 5. Phase D2 — SMC invalidation (buffer reuse / recompiles)

Unchanged from v1, with today's names:

1. **Store-path dirty mark.** In `st_i`/the ST arms: if the store address hits a page with
   translated dynamic uops, set a device bitmap bit (4 KB pages; one range-compare + a
   idempotent byte store; gate with a per-page "has-translations" bitmap so TCC's
   compile-phase stores into *not-yet-executed* buffers cost nothing and invalidate
   nothing).
2. **Between launches** the host scans the bitmap; dirty page → reset that page's `pc2uop`
   words to `RC_BADUOP` (uops themselves become garbage-but-unreachable; space is
   reclaimed only by a full rebuild — acceptable: TCC recompiles are rare events). The
   fixup list entries pointing into the page are dropped.
3. The **static** span keeps its no-write assumption (sp-proof and exec depend on it); a
   store into the *static* code span should disable exec + force a full retranslate (rare,
   loud `fprintf` — TinyCC never does this).

**Gate:** extend the fuzz phase: overwrite the stage-2 region with a second random program
mid-run, re-enter, compare. Plus a CudaTinyCC variant that compiles the same source twice
into the same buffer.

## 6. Non-goals (decided)

- **exec_block for dynamic code.** A region build costs seconds of ptxas wall (even
  parallelized, 7c65f67) — per TCC compile that's worse than interpreting. The hybrid
  already mixes engines by pc; dynamic code simply stays on the interpreter tier. If a JIT
  guest ever runs ONE buffer hot for minutes, revisit with a "promote after N entries"
  counter.
- **Fused arms inside dynamic blocks** (D1.4) until the plain version is gated.
- **True per-store SMC inside a single launch** (store→execute within one batch without a
  host trip). The batch boundary is the consistency point; TCC flushes/compiles long before
  executing. Document as a known relaxation vs hardware.

## 7. Order & effort

| step | size | risk |
|---|---|---|
| D1.1 full-RAM pc map | small | low (memory math only) |
| D1.2 growable uop buffers | medium | low |
| D1.3 unresolved-taken recovery + fixups | **the real work** | medium — kernel BR/JAL arms + driver loop; fuzz4 extension is the safety net |
| D1.4 harness `--rvcud` + output gate | small | — |
| D2 dirty bitmap + invalidation | medium | low once D1 is solid |

Worth doing when: TinyCC-class guests matter for more than the demo, or when DOOM-class
guests start shipping code the static span misses. The base kernel remains the universal
fallback throughout — `--no-jit` style, exactly like CudaVoxel.
