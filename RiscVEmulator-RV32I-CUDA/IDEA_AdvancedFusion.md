# Advanced uop fusion — driving single-thread Doom toward 10 MIPS

## Where the time goes (measured, RTX 3080)
A single Doom instance is **one GPU thread**: serial-dependency-bound, not memory- or
throughput-bound. ncu on a steady launch: **SM 0.13%, L1 hit 99.99%, DRAM 1.45%, long-scoreboard
(memory) stall 0.18** → the GPU is ~99.9% idle and the thread is just walking a long chain of
dependent instructions with nothing to hide latency behind. So wall-time ≈

    (dynamic uop count) × (SASS instrs per uop, dominated by shared-regfile LDS/STS)

Two levers, both about **doing less per guest instruction**:
1. **Fewer uops** — fuse multiple RV32I instrs into one advanced-uarch uop.
2. **Fewer shared-regfile ops per uop** — keep transient values in CUDA registers
   (*virtual registers*) instead of round-tripping the 33-stride shared regfile (the
   documented per-uop bottleneck).

### Hot-path histogram (per-uop execution counts, cold-boot-to-frame-2)
- **~33%** byte copy / column-blit loops: `lb rt,0(src); sb rt,0(dst); addi src,Ks; addi dst,Kd; bne`
  (horizontal memcpy: Ks=Kd=1; vertical column blit: Ks=1, Kd=320=screen width).
- **~21%** bit-serial software divide (`__udivsi3`/`__divsi3`, rv32i has no hardware divide).
  *Deliberately out of scope* — user's call; widening the copies is the lever.
- remainder: distributed pointer/array/struct code (address-gen `add`/`slli`+`add` feeding loads/stores).

## Implemented
| uop | fuses | status |
|---|---|---|
| `LOADPI`/`STOREPI` | `lX rd,0(base); addi base,base,K` → load/store + post-increment | **validated** (gate ✓, renders) |
| `COPYPI` | `lb rt,0(src); sb rt,0(dst); addi src,Ks; addi dst,Kd` → copy step, dual post-inc | **validated** (gate ✓, renders; note: must write `rt`, the loaded value, since it can be live) |
| `LDX`/`STX` | `add rt,ra,rb; lX/sX …(rt)` → indexed mem op; address temp `rt` virtualized (never hits shared regfile) | **UNTESTED — validate gate + render when GPU free** |

Key correctness lesson (COPYPI): a fused uop must still produce every architectural register the
original sequence did, unless provably dead. Don't assume a temp is dead — either prove it via
liveness, or write it. (COPYPI initially skipped the loaded-value write → corrupted Doom.)

## Designed, not yet built (in rough value order)

### 1. Word-widening COPYLOOP (the "integer copy" — biggest single lever)
Recognize the whole **forward unit-stride byte-copy loop** and run it inside one `RC_COPYLOOP` uop,
copying **4 bytes per step when safe**. This removes BOTH the per-byte dispatch AND 3/4 of the
memory ops. `ld_i`/`st_i` already handle unaligned word access (they span two interleaved words), so
widening works at any alignment — aligned is just cheaper. Scope it tightly (this is where blind bugs
live): only `Ks==Kd==+1`, byte width, `bne counter,lim` back-edge to the loop header.

**Recognition** (`rvcud_try_copyloop`, before `try_copy`): at loop-header `w`, a copy step
`lbu/lb rt,0(src); sb rt,0(dst); addi src,1; addi dst,1` (4 words, any order — reuse `try_copy`'s
matcher, require unit stride + byte) immediately followed at `w+4` by `bne creg,rlim, →w`
(`creg ∈ {src,dst}`, `rlim` the other operand). Require `src,dst,rlim,rt` all distinct. Consume 5 words.

**Encoding**: `w0 = {cls, rd=rt, rs1=src, rs2=dst, f3=load-width(0|4), sra-bit=counter_is_dst}`;
`w1 = (rlim<<24) | fallthrough_uop_idx`. `tgt = w+5` (fall-through word); Pass-3 bakes
`pc2uop[w+5]` into `w1[23:0]` (sentinel `0xFFFFFF` if unresolved).

**Kernel** (do-while mirrors the guest's body-then-branch; entered only with `gi<budget`):
```
s=u1; d=u2; L=regs[w1>>24 &31]; cdst=(w0>>25)&1; signed=(f3==0);
do {
  cnt = cdst? d : s;  rem = L - cnt;  adiff = |d-s|;
  if (rem>=4 && adiff>=4) { v=ld32(s); st32(d,v); s+=4; d+=4; gi+=20; }   // word, overlap-safe
  else                    { b=ld8(s);  st8(d,b);  s+=1; d+=1; gi+=5;  }   // byte tail / overlap
} while ((cdst?d:s) != L && gi < budget);
regs[rt] = signed ? sext(ld8(s-1)) : ld8(s-1);   // last loaded byte (rt may be live)
regs[src]=s; regs[dst]=d;
ui = ((cdst?d:s)==L) ? (w1&0xFFFFFF) : ui;        // done → fall-through; else budget-cut → resume here
continue;
```
**Why each guard matters / where bugs hide:**
- `rem>=4` prevents overshooting `L` (counter must land exactly on `L` to match the `bne`).
- `adiff>=4` is the memmove-overlap guard — a word read-then-write differs from byte-by-byte only
  when regions overlap within 4 bytes; `d-s` is invariant (both +1) so it's checkable per step.
- exact `gi` (5/byte, 20/word) keeps the rv32i verify-gate bit-identical: it runs the same retired
  count → same byte count → same memory.
- budget-cut leaves `ui` at the COPYLOOP uop and `src/dst` partially advanced → the driver
  re-launches and the do-while resumes from the live pointer state. No separate resume bookkeeping.
- fall-through sentinel `0xFFFFFF`: if `w+5` isn't a uop leader at bake time, fall back to NOT
  emitting COPYLOOP (emit COPYPI + 1:1 bne) — never halt on an unresolved fall-through.
Expected: ~4× on the horizontal-copy share (~15% of execution) plus dispatch elimination. The
vertical column blit (stride 320) stays COPYPI (can't widen non-contiguous dst).

**Validate carefully**: this is a self-looping, budget-resumable uop — test the gate (does the data
guest hit it?), the title + an in-game frame, AND a deliberately budget-split run (small StepN) to
exercise mid-loop resume.

### 2. Scaled-indexed load/store `LDXS`/`STXS`
`slli rt,ri,k; add rt2,ra,rt; lX rd,imm(rt2)` → `rd = mem[ra + (ri<<k) + imm]` in one uop.
The canonical `array[i]` for element sizes 2/4/8. Two virtual registers (`rt`, `rt2`) eliminated.
Encode `k` (hi16 of w1) + `imm` (lo16). Recognition must guard the aliasing cases
(`rt`/`rt2` dead-or-overwritten, `ra`/`ri` unmodified) — 3-instruction window, so build + test
carefully (untested 3-op patterns are where bugs hide).

### 3. Virtual-register ALU-pair fusion (generalized)
Any `op1 rt,…; op2 rd,rt,…` where `rt` is dead after `op2` → one uop computing `t=op1(...)` in a
CUDA register and only `rd` to shared. `LEA`/`XSH`/`MULADD`/`LDX` are special cases; a general
2-ALU uop (`rd = op2(op1(rs1,A), B)`) with a small op-pair selector would catch the long dependent
ALU chains clang emits. Encoding two ops + operands in 64 bits is the constraint — pick the top
handful of `(op1,op2)` pairs seen in the histogram rather than a general form.

### 4. Shadow registers for `sp`/`gp`/`tp`/`x0`
A few architectural registers are touched constantly (stack pointer `x2`, global pointer `x3`).
Keeping them in dedicated CUDA-register variables (a "shadow") avoids shared-regfile traffic — but
the regfile is indexed by a runtime 5-bit field, so every access would need an `idx==2 ? shadow :
shared` select. Likely a net loss unless the select is cheaper than the LDS it replaces; measure
before committing. `x0` is already free (reads as 0, writes dropped).

### 5. uint4 uop packing (reduce loads per uop)
Today each uop reads `uops[ui].{x,y}` (8 B, one line) + `uw[ui]` (separate line) + occasionally
`uop2pc[ui]`. Packing `{w0,w1,weight,uop2pc}` into one 16-B `uint4` → a single `LD.128`. Prior
experiments found this neutral on the *throughput* bench (latency hidden across warps); on the
*single-thread* latency-bound regime it may help (un-hidden), but the ncu profile shows low memory
stall, so the upside is uncertain. Doubles uop memory → re-check the 4096-core throughput gate.
Lower priority than 1–3.

## Validation protocol (every change)
1. `CudaBench.exe` → `rvcud bit-identical ✓` (exact retired-count match on compute/data/diverge).
2. `CudaDoom --rvcud --shot frame1.png --shot-frame 1` → title renders pixel-perfect (60,589 non-black px).
3. `CudaDoom --rvcud --shot demo.png --shot-frame 5` → an in-game frame renders (exercises the blits).
4. `CudaDoom --rvcud --bench` → steady-state MIPS + the clock-independent `guest-instr/uop` ratio.
   **GPU clock swings 1410↔2100 MHz under load/heat and can't be locked without admin — compare the
   clock-independent fusion ratio, or best-of-N at matched clock with no other GPU app running.**
