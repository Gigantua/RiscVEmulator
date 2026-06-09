# IDEA: Self-Modifying / JIT Guests on the rvcud Transcompiled Core

**Status:** deferred (design note). rvcud is the *static* fast path; the base live-fetch
core is the universal one. This documents how rvcud would grow into a dynamic binary
translator (DBT) so it can also run runtime-compiled and self-modifying guests.

---

## 1. The problem

We have two CUDA execution cores for the RV32I guest:

| core | how it fetches | speed | runs |
|---|---|---|---|
| **base** (`rv32i_kernel`) | live, per-instruction, from the core's own RAM (`ld_i` + inline `decode_imm`) | 1× | **anything** — static, indirect-heavy (DOOM), self-modifying / JIT (TinyCC) |
| **rvcud** (`rvcud_kernel`) | a pre-built **uop stream** + `pc2uop[]` map, produced once by `rvcud_build` in `cuda_rvcud_set_code` | ~4.6× compute, ~1.8× data (bit-identical) | only **static, low-indirection** guests (the benchmarks) |

rvcud is fast because it translates the guest **once, ahead of launch**, fusing
multiple guest instructions into one uop. The price: it only knows about code that
existed at translate time. Two things break it:

1. **Code that appears after launch** — a JIT guest (TinyCC) compiles C → RV32I into
   guest RAM *at runtime*, then `jalr`s into it. rvcud has no uop for that address.
2. **Code that is overwritten after translation** — true self-modifying code; the
   cached uops become stale.

(There is also a third, separate issue that blocks even *static* DOOM: indirect-call
targets aren't uop leaders — see §5.)

When rvcud reaches untranslated code, `pc2uop[target] == RC_BADUOP` and the kernel sets
`HALT_BIT` and stops. Today that just looks like a hang.

---

## 2. Key enabler: every guest write goes through the core

On a CPU, a DBT detects self-modifying code with the MMU: mark the code page
read-only, take a write fault, invalidate. **CUDA has no such mechanism** — device
memory has no kernel-trappable write protection, and unified-memory page faults are for
host↔device migration, not per-write notification.

We don't need it. **Every guest load/store already funnels through `ld_i` / `st_i` in
the kernel**, so the interpreter itself is the "MMU". We can detect both new-code
execution and code overwrites in software, cheaply, at points we already control.

---

## 3. Translate-on-miss (handles *new* code) — lazy, per-basic-block

You never need to know "how big is the region to translate". You translate **one basic
block at a time**, bounded by control flow, discovered by decoding. This is exactly how
QEMU/TCG works.

**Trigger.** rvcud already detects the miss: a `JALR` to an untranslated target, or a
direct branch/`jal` whose baked target is the unresolved sentinel (`0xFFFFFFFF`), makes
`ui` go negative → the run loop exits (halt). We just need to **surface the faulting
guest pc** to the host (e.g. stash it in `g_ret` / a status word instead of only setting
`HALT_BIT`).

**Translate one block.** From the miss `pc`, decode forward (RV32I is fixed 4-byte) and
emit uops until the first **control-transfer instruction**, inclusive:
- `jal` / `jalr` (unconditional) → block ends,
- conditional `branch` → block ends (two successors),
- (optional) an already-translated instruction → stop, we've rejoined known code.

The block's *extent is the decode itself* — entry → first terminator. No size hint, no
symbols needed. Fusion still applies *within* the block (it never crosses control flow
anyway).

**Wire it up.** Append the new uops to the stream, set `pc2uop[entry]`, and patch the
branch/`jalr` that pointed here. Direct targets that are still untranslated are baked as
the unresolved sentinel → taking them faults → translate *those* blocks on their own
misses. Each reachable block is translated the first time it is entered.

**Loop:** `run → halt-on-miss → host reads miss pc → translate that block from live RAM →
append uops + set pc2uop + patch predecessor → resume.`

This is batch-friendly: misses surface at `StepN` boundaries where the host already has
control.

---

## 4. SMC invalidation (handles *overwritten* code) — store-path dirty bitmap

New code is covered by §3. **Overwriting** already-translated code needs invalidation.
Do it in the store path:

- Keep a device-side **dirty bitmap**, one bit per small code page (e.g. 4 KB → bit =
  `addr >> 12`; finer pages = less re-translation per hit, bigger bitmap).
- In `st_i` (the store handler), after the address is known:
  ```
  if (addr - codeLo < codeSpan) dirty[(addr - codeLo) >> PAGE_SHIFT] = 1;
  ```
  A single range compare, and a write only when a store actually lands in the code span
  (rare for ordinary data stores). Branchless, no atomics needed (idempotent set).
- **Between launches**, the host scans the bitmap; for each dirty page it reads that page
  back from `g_mem` (`cuda_rv32i_read_mem`), re-translates it, patches uops + `pc2uop`,
  clears the bit, resumes.

`addr >> 12` into a bitmap is about as cheap as SMC detection gets — and it's only paid
by stores that hit the code range.

Execution-miss (§3) and SMC (§4) are **orthogonal**: misses = jumping to *new* code,
caught by the `BADUOP` halt; SMC = clobbering *old* code, caught by the store flag.
Together they form a complete lazy DBT.

---

## 5. Separate blocker: indirect-call leaders (breaks even static DOOM)

Independent of JIT/SMC, rvcud can't run DOOM today. `rvcud_build` marks uop **leaders**
only at direct branch/jump targets, the entry, and fall-throughs. A function reached
**only through a function pointer** (`jalr`) has no leader at its entry → `pc2uop` =
`BADUOP` → the indirect call halts. DOOM is saturated with function-pointer dispatch
(think/action tables), so it halts early; the benchmark guests have ~none, so they run.

The translate-on-miss machinery in §3 **also fixes this**: an indirect call to an
untranslated entry is just a miss → translate that block on demand. So §3 is the higher-
value piece — it unblocks both DOOM (static indirection) and TinyCC (runtime code).
§4 (SMC) is only needed for guests that truly overwrite live code.

---

## 6. Scope / required changes

1. **Surface the miss pc** — kernel writes the faulting guest pc to a status word on halt
   (small change to the `BADUOP`/`HALT_BIT` paths).
2. **Incremental `rvcud_build`** — translate a *single block* from a given pc and *append*
   to the uop / `pc2uop` / `uop2pc` / weight buffers, instead of whole-image-once. Buffers
   must be growable (over-allocate + realloc, or a free-list).
3. **Predecessor patching** — when a block is translated, fix up the unresolved branch
   targets that point at it.
4. **(SMC only) store-path dirty bitmap** + between-launch scan/readback/re-translate.
5. **Host driver** in `CudaEmulator.StepN` — on a miss-halt, run translate-on-miss and
   resume within the same `StepN`.

---

## 7. Tradeoffs / when to use which core

- **rvcud (static)** — fastest; use for guests whose code is fixed after entry and that
  don't lean on pointer-only dispatch (the benchmark class). No DBT machinery, no SMC tax.
- **rvcud + translate-on-miss** — would run DOOM and any code-after-launch guest; pays a
  one-time translation cost per first-executed block (amortized over the run).
- **rvcud + SMC bitmap** — additionally correct for guests that overwrite live code
  (TinyCC reusing a code buffer); pays a per-store range-check on the hot path.
- **base live-fetch core** — always correct for everything (re-reads live RAM each fetch),
  zero translation machinery; the universal fallback. This is what runs DOOM and TinyCC
  today.

**Recommendation:** keep rvcud as the static fast path and the base core as the universal
one. Implement §3 (translate-on-miss) first if/when rvcud needs to cover DOOM-class or JIT
guests — it's the bigger unlock and is correctness-safe. Add §4 (SMC) only for guests that
actually self-modify.
