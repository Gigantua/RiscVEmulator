# Making call-heavy guests (DOOM) fast under the CUDA JIT — soundness-first

## Hard constraint (drives everything)

The guest binary is arbitrary **vanilla RV32I** and **must never be modified or
assumed to behave a certain way**. Special *clang flags* when WE build a test
guest are fine, but JIT **correctness may never depend on them** — the interpreter
seam stays the backstop for anything the JIT doesn't cover.

### What this rules OUT (corrections to the earlier plan)
- **No mapping `JAL`→device `call` / `JALR`→device `ret`.** RV32I control flow is
  not guaranteed LIFO-nested. `setjmp`/`longjmp` (PureDOOM's `I_Error`!), tail
  calls, coroutines, and computed jumps through `ra` all violate it; a hardware
  call/ret stack would desync or overflow. `JAL`/`JALR` are *only* "rd = pc+4;
  jump" — nothing more.
- **No ABI / live-register analysis.** We cannot assume the guest honors
  caller/callee-saved conventions, so at every dynamic transfer **all 32 registers
  are potentially live**.

The current sliced JIT is already SOUND under these rules (JALR returns pc to a
trampoline; regs all kept live in memory). It is just slow. The speed-up must come
from a *sound* mechanism.

## Why DOOM is slow (unchanged): two costs at transfer boundaries
1. **Memory-resident register file** — slices pass regs as `uint32_t* R` (local
   memory). The 180× path keeps `R[32]` in real registers, but only because it is
   ONE function.
2. **Trampoline round-trip per transfer** — every cross-slice branch / JALR bounces
   pc → kernel loop → `jit_dispatch` → `switch(slice)` → `call` → `switch(pc)`.

## Sound mechanism for both: register-resident superblock + `brx.idx`

The ONLY way to keep regs in SASS registers across an *arbitrary* control transfer
is to make the transfer a **branch inside one function**, not a call. So:

- Collapse a body of guest blocks into **one `__device__` function**; guest regs
  are 32 **locals** (register-resident); every block is a label.
- Static targets (direct branch / JAL / JALR with statically-known target) →
  `bra L_target` (chained).
- Dynamic targets (JALR, indirect) → PTX **`brx.idx`** over a `.branchtargets`
  list of all in-region block leaders, indexed by a guest-pc→index map. O(1),
  stays in the function, **regs stay in registers across it**, and it is SOUND for
  any control flow (it's just a jump; no stack).
- Anything not in the region / unresolved → exit to the sound trampoline / interp
  seam (correctness backstop). All 32 regs spill to the shared file on exit and
  reload on re-entry — paid only when leaving the region.

This eliminates BOTH costs *on the hot path* without any call/ret or ABI
assumption: a hot loop that calls and returns among hot blocks does so via
`bra`/`brx.idx` inside one function → registers never touch memory, dispatch is
O(1), control flow is whatever the guest actually does.

The only blocker is **compile-time/feasibility of a large single function** (cicc
is single-threaded; a 150k-instruction whole-program function is infeasible). So
we size the register-resident region to the **hot set**, not the whole program.

## Profile-guided hot-region formation (guest-agnostic)

We don't analyze the guest statically (we can't trust it) — we **observe** it:
1. Run briefly under the interpreter / current sliced JIT with a lightweight
   **hot-pc histogram** (a managed `uint32 count[]` indexed by (pc-lo)/4, bumped
   at block leaders).
2. Pick the hot set (covers ~the cycles, a few k instructions for DOOM's
   render/game-tic loops).
3. Emit ONE register-resident `brx.idx` superblock for the hot set; everything
   else stays on the sound sliced/interp path. Recompile (cached) and switch.
This is a standard tiered/trace JIT and is robust to ANY guest.

## Phased plan (each bit-exact-gated vs the interpreter)

**Phase 0 — Profiling + feasibility.** Add the hot-pc histogram to the kernel;
dump DOOM's hot set + JAL/JALR density. Measure how large a single register-
resident function cicc will compile in acceptable time → fixes the region budget. 

**Phase 1 — `brx.idx` dynamic dispatch (sound, immediate win).** Replace the O(n)
DISP compare-chain / slice+pc double-switch with a `brx.idx` + guest-pc→index map
(dense LUT if pc range is dense, else sorted/binary-search). Cheaper dispatch with
zero soundness change; helps every dynamic transfer including the existing path.

**Phase 2 — Register-resident hot superblock (the big one).** Build the hot set as
one device function, regs as locals, `bra`/`brx.idx` internal transfers, exit to
the sound path on leaving the set. Hot call/return cycles become register-resident
+ O(1). Expect the multi-× that closes most of the gap to the 180× regime.

**Phase 3 — Grow the region toward whole-program** as far as cicc tolerates
(low opt `-Xcicc -O1`); if/when a guest's whole hot core fits one function it
approaches the small-guest regime. Cold code stays on the sound path forever.

**Phase 4 — Dense pc→label LUT + back-edge-only budget** to make `brx.idx`
dispatch and hang-safety as cheap as possible.

## Allowed clang flags (improve JIT-friendliness; correctness-INDEPENDENT)

We may build our test guests with flags that increase static branch-target
resolution and shrink code (so more chains, smaller hot function, faster compile)
— but the JIT must stay correct without them:
- `-fno-jump-tables` — fewer computed `JALR` (more static targets recovered);
- `-Os`/`-Oz` — smaller hot region → compiles faster, more fits one function;
- `-mno-relax`, `-fno-optimize-sibling-calls` — fewer tail-call/relaxation oddities
  so static recovery covers more (we still handle the rest soundly).

## Soundness invariants (must hold for ANY vanilla RV32I)
- `JAL`/`JALR` only set `rd=pc+4` and jump — never a hardware call/ret.
- All 32 registers live at every dynamic transfer (no dead-reg/ABI assumption).
- Any unrecovered/indirect target → interpreter seam (bit-exact backstop).
- No device-call stack ⇒ no LIFO requirement, no `cudaLimitStackSize` / recursion
  concerns; longjmp/coroutines/tail-calls just work.
- Validation: deterministic DOOM frame-hash (interp vs JIT) + the jit_test gate.

## Expected outcome
Phase 1 cheapens all dispatch; Phase 2 makes DOOM's hot call/return loops
register-resident and O(1)-dispatched — sound for arbitrary RV32I — lifting it well
above today's ~1.4 MIPS toward the register-resident regime, with cold code always
falling back to the proven interpreter.
