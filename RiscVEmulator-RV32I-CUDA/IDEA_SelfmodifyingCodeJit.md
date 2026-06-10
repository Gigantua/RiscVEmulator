# PLAN: TinyCC / runtime-generated code on the rvcud fast path — full design

**Status:** planned, v3 (2026-06), implementation-grade. v1 predated exec_block and
translate-on-miss; v2 sketched the phases; v3 is the full design, centered on the dirty-page
system for overwritten code.

---

## 1. Goal and current state

Run guests that **generate and overwrite code at runtime** (TinyCC: compile C → RV32I into a
heap buffer, `jalr` into it, possibly recompile into the same buffer) on the rvcud uop core
instead of the ~1× base kernel that carries them today (`Examples/CudaTinyCC` never sets
`UseRvcud`).

Already built and reused as-is:
- **Translate-on-miss** (`rvcud_translate_miss`): lazy 1:1 block translation at a missed JALR
  target, driver-resumed (`resume_pc`). Unblocked DOOM's function pointers.
- **Halt-pc preservation** (b88a617), `cuda_rv32i_get_pc/get_reg`.
- **Differential fuzzing** of the full translator+interpreter pipeline (`cuda_rvcud_fuzz`,
  fwd + jalr phases), `RVCUD_FUSEMASK`, `RVCUD_TRACE`.

Known-broken pieces this plan fixes:
- Generated code lives in the **heap, outside `g_pc2words`** — unmappable, not just
  untranslated (`rvcud_translate_miss` bounds-checks and bails).
- Miss-translated blocks bake unresolved direct targets as `0xFFFFFFFF`; **taking one kills
  the core** (`ui=0xFFFFFFFF` → OOB `uop2pc` read). Measured live in the voxel span
  postmortem. Fine for DOOM's straight-line stubs, fatal for a JIT'd loop.
- **No invalidation**: overwriting translated code leaves stale uops forever.

## 2. The consistency contract — `fence.i` is the sync point

RISC-V already defines the answer: a guest that writes instructions MUST execute `fence.i`
before executing them. We currently decode `op 0x0F` (fence / fence.i) as a NOP. The design:

- **Stores mark pages dirty** (cheap, cold-path — §4).
- **`fence.i` exits the launch** (only when dirty pages exist) → the host **invalidates**
  every dirty page: reset its `pc2uop` words to `RC_BADUOP`, **unpatch** all recorded edges
  pointing into it, clear the bits → resume.
- The next jump into an invalidated page is an ordinary **miss → translate-before-execute**.
  This is exactly "mark dirty on write, retranslate on next entry", with the re-entry check
  costing nothing: invalidation makes the existing miss machinery do it.
- **Batch boundary as belt-and-braces:** the host also scans the dirty bitmap between
  launches, so an unfenced guest is still correct whenever write→execute crosses a `StepN`
  boundary. Unfenced write→execute *within one launch* reads stale translations — the same
  thing real hardware does without `fence.i`; documented relaxation, not a bug.
- **Guest patch required:** TCC's RISC-V port never flushes the icache (`tccrun.c` only does
  for ARM). Our vendored guest stub (`Examples/TinyCC/Programs/stubs.c` —
  `mprotect`/`set_pages_executable` path) gets a one-liner `asm volatile("fence.i")`. That
  also makes the guest correct on real hardware.

Why this beats per-jump dirty checks: no engine ever tests dirtiness on the hot dispatch
path (JALR, XDISP, baked branch edges all stay untouched); the only new kernel cost is on
**stores**, and only the range compare.

## 3. Data structures (exact)

| structure | size @ 16 MB RAM | notes |
|---|---|---|
| `pc2uop` widened to full RAM | 4 M words × 4 B = **16 MB** device | today: code span only (~430 KB for Doom). `uop2pc` grows with uops, not with the map. Allocate once; words init `RC_BADUOP`. |
| uop buffers (`g_uops/g_uw/g_uop2pc` + host mirrors) | grow in **64 K-uop chunks** | today capacity is `nuops + 2N + 16`, sized for static re-emission only. Growth happens host-side between launches (realloc device arrays, memcpy mirrors — same pattern `set_code` uses). |
| **dirty bitmap** `g_dirty` | 4 KB pages → 4096 bits = **512 B** device | one byte per page is simpler than bit ops in PTX/SASS: **4 KB** array, byte = page dirtied. Idempotent stores, no atomics. |
| **translated bitmap** (host-only) | 4 KB pages → 512 B host | which pages contain dynamic translations. Gates invalidation cost; also maintains the device-side **watch window** below. |
| **watch window** `[g_wlo, g_whi)` | 2 × u32, kernel params / `__constant__` | tight bounding range of pages that have dynamic translations. Stores compare against this, not the bitmap — empty window (`wlo=0xFFFFFFFF`) makes the check dead for static guests. Updated by the host whenever it translates/invalidates dynamic pages. |
| **edge fixup list** (host-only) | `std::vector<{uopIdx, targetWord}>` | every direct branch/jal baked unresolved or patched into dynamic code. Used for (a) late patching when the target gets translated, (b) **unpatching** when the target page is invalidated. |
| per-page **uop leak ledger** (host-only) | optional | invalidated uops become unreachable garbage in the stream. TCC recompiles are rare; reclaim only via full rebuild when waste exceeds e.g. 25% of capacity. |

## 4. Kernel/engine changes

### 4.1 Store-path dirty marking — every engine that stores

The check is two ALU ops on the hot path, with the bitmap write in a cold section
(r6 branch-over pattern):

```
  ; addr in %a (guest address), window in %wlo/%whi
  setp.ge.u32 %p, %a, %wlo;  setp.lt.and.u32 %p, %a, %whi, %p;
  @%p bra DIRTY_<site>;          ; not-taken on every store of a static guest
RET_<site>: ...
  ; cold section:
DIRTY_<site>: shr  %t, %a, 12;  st.global.u8 [g_dirty + %t], 1;  bra RET_<site>;
```

Sites that need it:
- **Interpreter:** `RC_ST`, `RC_STX`, `RC_STXS`, `RC_STOREPI`, `RC_COPYPI`, and every fused
  loop arm that stores (`MEMSET`, `WORDFILL`, `COPYLOOP[S/T]`, `PALEXP`, `TEXSPAN`,
  `TEXCOL`). Fused arms can do ONE range check per loop entry against `[dst, dst+len)`
  instead of per element.
- **exec_block:** `rvx_emit`'s store cases (aligned path + cold fallback) and the helper
  units (`xcopy`, `xfill`, `xpal`) — helpers take the window as two extra params and do one
  whole-range check per call.
- **Host writes** (`cuda_rv32i_write_mem`, iobox staging): host-side check, trivial.

A store into the **static** code span (separate compare, only when the static span has exec
coverage) is loud: disable exec (`g_xblk_ok=0`), force full retranslate at next boundary.
TinyCC never does this; correctness demands the check exist.

Perf budget: +2 issue slots per scalar store, window empty for Doom/voxel. **Gate: 2-pair
interleaved ttf90 must be neutral (±1%); if not, bake the check only when
`RVX_DYNCODE=1`** (CudaTinyCC sets it; exec PTX and the interpreter branch on a launch flag).

### 4.2 `fence.i` exit

`op 0x0F` with `f3==1` (fence.i — plain `fence` stays a NOP): if any dirty page exists
(cheap: kernel keeps a `g_dirty_any` byte the cold sections also set), exit the launch with
a new stop reason (`resume_pc = pc+4`, status flag in the ret word). Static guests never
take it.

### 4.3 Unresolved-edge recovery (the D1.3 fix — prerequisite for everything)

Today: `RC_BR/RC_JAL` with `w1==0xFFFFFFFF` poisons `ui`. Change both arms:

```
if (w1 == 0xFFFFFFFFu) { resume_pc = <guest target pc>; break; }   // exit, don't poison
```

The target pc: bake it instead of the sentinel — use a **second sentinel encoding**: set
bit 31 of `w1` and store the *target word index* in bits 30:0 (uop indices stay < 2^30, so
bit 31 cleanly means "unresolved, payload = target word"). Costs one extra compare on the
taken path of baked branches (`(int32_t)w1 < 0`), nothing on fall-through.

Driver loop on that exit: translate the block at the target (now always in-map after §3),
**patch** every fixup-list edge that resolves, record new unresolved edges, resume. Each
edge faults at most once.

### 4.4 Dynamic-block translation policy

`rvcud_translate_miss` as today (straight-line 1:1, stop at jal/jalr/rejoin), with:
- bounds check against full RAM instead of the static span,
- fixup-list recording (§4.3),
- **fusion off** in dynamic blocks initially (`FUSEMASK`-style internal mask) — TCC output
  is naive and would fuse well, but that's a measured A/B after correctness,
- buffer-growth check before appending (§3); if full, return "grow" to the driver, which
  reallocs and retries.

exec_block **never** covers dynamic code (non-goal — a region build costs seconds of ptxas
wall even parallelized per 7c65f67; interpreting JIT output is the right tier). If a guest
ever runs one buffer hot for minutes, revisit with an entry counter + promote.

## 5. Host driver (cuda_rvcud_step_all) — new exit reasons

```
loop:
  launch (exec or interp as today)
  switch (stop reason):
    halted                  → return
    budget                  → return/continue as today
    JALR miss (existing)    → translate block @ resume_pc, patch fixups, continue
    unresolved edge (new)   → same as miss
    fence.i + dirty (new)   → invalidate dirty pages:
                                for each dirty page P with translations:
                                  pc2uop[P's words] = RC_BADUOP
                                  unpatch fixup edges targeting P  (w1 → unresolved sentinel)
                                  drop fixups originating in P
                                clear bits; shrink/recompute watch window; continue
  between StepN launches: same invalidation pass if any dirty bit set (unfenced fallback)
```

All of it stays inside the native driver; `CudaEmulator` only grows a `DynamicCode`
property (sets `RVX_DYNCODE` behavior) and CudaTinyCC sets `UseRvcud + DynamicCode`.

## 6. Multi-core note

Dynamic translation mutates shared structures (`pc2uop`, uop stream) that all cores read.
The driver only translates **between launches** (no core is running) — already true for
translate-on-miss today. Dirty bits set by any core are a union — correct. `nc>1` TinyCC is
out of scope but nothing here breaks it structurally.

## 7. Validation — gates before any perf claim

1. **fuzz5 (extend `cuda_rvcud_fuzz`): the SMC phase.** Per program: (a) generate static
   program A containing a "writer" loop that the HOST pre-computes: A stores program B's
   words into a heap address, executes `fence.i`, jalrs to B; B computes, returns, A stores
   program C's words **over the same buffer**, `fence.i`, jalr again. Compare final
   regs/mem/pc against `rvx_ref_step` running the same image. Randomize B/C with the
   existing M-aware generator. This exercises: full-RAM map, miss-translate, unresolved
   edges inside B/C (loops!), dirty marking, fence invalidation, re-translation, fixup
   unpatching.
2. **fuzz5b — unfenced variant:** writer omits `fence.i` but the harness forces a `StepN`
   boundary between write and call (chunked budgets) — validates the boundary fallback.
3. **CudaTinyCC gate:** `--rvcud` output (fib/primes/mandel text) byte-identical vs the
   base kernel run; plus a `--twice` mode that compiles the same source twice into the same
   state (exercises real TCC buffer reuse).
4. **Static-guest regression:** CudaBench both bit-identical lines, CudaMath bit-exact,
   Doom 2-pair ttf90 neutral (the store-check budget, §4.1), voxel `--bench` neutral.

## 8. Phases, order, effort

| phase | items | size | risk |
|---|---|---|---|
| **D1a** | full-RAM `pc2uop` (§3), growable uop buffers | S–M | low — memory plumbing |
| **D1b** | unresolved-edge sentinel + recovery + fixup list (§4.3, §5) | **M — the core work** | medium; fuzz4 already covers the static paths, fuzz5 covers the rest |
| **D1c** | CudaTinyCC `--rvcud` + output gate; measure | S | — |
| **D2a** | dirty bitmap + watch window + store-path checks in all engines (§4.1) | M | medium — many sites; Doom A/B decides always-on vs `RVX_DYNCODE` |
| **D2b** | `fence.i` exit + host invalidation/unpatch + guest stub `fence.i` (§4.2, §5) | S–M | low once D2a lands |
| **D2c** | fuzz5/5b + `--twice` gate | S | — |
| later | fusion in dynamic blocks; uop-space reclamation; promote-to-exec counters | — | only with measured need |

D1 alone runs the TinyCC demo correctly **if** each compile lands in a fresh buffer (first
execution of any page is always correct — translation happens at first entry). D2 is what
makes buffer reuse/recompiles correct. Ship them together; D1-only is a footgun.

## 9. Expected outcome

- TinyCC's own 400 KB compiler binary runs on rvcud + exec (it's static — full fast path,
  M-extension capable if we move the guest to rv32im like voxel).
- Generated code runs 1:1-interpreted on rvcud — roughly the interpreter's per-uop rate vs
  the base kernel's per-instruction rate; the demo's mandelbrot loop should see the largest
  win. Numbers to be measured at D1c, not promised.
- Doom/voxel/bench: zero behavior change, store-check cost gated to ±1% or compiled out.
