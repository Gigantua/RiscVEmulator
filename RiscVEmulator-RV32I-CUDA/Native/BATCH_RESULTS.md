# Batch round: 14 parallel units → measured serially → winners merged (branch `batch-integration`)

14 background agents implemented one unit each in isolated worktrees off `rv32i_on_cuda` @ 1c4b4d3
(all local, never pushed). Every unit passed the bit-identical gate at commit time. The coordinator
then built one DLL per unit and measured them sequentially on a quiet GPU (deterministic
`--ttf 30` ×2, fixed 81.0M guest steps, 2 GB process-commit killswitch per the user), merged the
winners, and re-verified the combination.

## Per-unit results (ttf30 best, base = 13.14 MIPS)

| Unit | ttf30 | Δ | Verdict |
|---|---:|---:|---|
| nc1flat — flat single-core ld_i/st_i (no interleave mul, no byte-RMW) | 13.69 | **+4.2%** | **MERGED** |
| rgbalut — PALEXP pre-expanded RGBA32 LUT | 13.62 | **+3.7%** | **MERGED** |
| fbword — TEXSPAN 4-pixel word-store packing | 13.58 | **+3.3%** | **MERGED** |
| dispatch-prefetch — exit-uop descriptor prefetch at loop entry | 13.42 | **+2.1%** | **MERGED** |
| cmapstage — colormap staged to local per loop | 13.24 | +0.8% | rejected (marginal; overlaps texmicro) |
| texmicro — last-texel/cmap word caches | 13.21 | +0.5% | rejected (marginal) |
| pixpipe — 2-deep texel software pipeline | 13.16 | ±0 | rejected (neutral — ptxas likely already pipelines) |
| sblk — JAL splice | KILLED | — | rejected: **ptxas memory explosion** (>2.6 GB in 2 s; the observed 12 GB) |
| xblk-cgmem — addr CSE + align proof | KILLED | — | rejected: ptxas explosion |
| xblk-cgctl — setp fusion + budget decimation | KILLED | — | rejected: ptxas explosion |
| xblk-loopemit — fused TEXSPAN/TEXCOL loops in PTX | KILLED | — | rejected: ptxas explosion |
| xblk-mailbox — pinned handoff mailbox | KILLED | — | rejected: ptxas explosion |
| xblk-norender — exclude fused loops from exec_block | KILLED | — | rejected: ptxas explosion |
| xblk-handback — interpreter→JIT handback | 2.63 | **−80%** | rejected: launch-storm (works, but each JALR→JIT transition pays a kernel round-trip) |

**Key systemic finding:** the exec_block JIT at RVX_MAXW=24000 sits exactly at ptxas's super-linear
memory knee (baseline process commit 1.38 GB). *Any* perturbation of the generated PTX — even
xblk-mailbox's 3 extra instructions — tips the assembler past 2.6 GB→multi-GB. The xblk family is
unshippable until RVX_MAXW is reduced (e.g. 8000) or codegen is split multi-function (MULTIMOD);
the units themselves are committed on their branches' history for that follow-up. xblk-handback
additionally needs cheap handoffs (conditional graphs / persistent kernel) before it can pay.

## Merged result (`batch-integration` = 1c4b4d3 + nc1flat + fbword + rgbalut + dispatch-prefetch)

Conflicts hand-resolved: render-arm bodies re-composed with nc1flat's `<T,NC1>` template params.

| | base 1c4b4d3 | merged | Δ |
|---|---:|---:|---:|
| gate | ✓ | ✓ | bit-identical |
| ttf30 (best of 3; spread 14.05–14.09) | 13.14 | **14.08** | **+7.2%** |
| ttf90 (best of 2) | 14.46 | **15.61** | **+8.0%** |
| render f1 | 60,589 px | **60,589 px** | exact |
| render f40 | 63,419 px | **63,419 px** | exact |

The four winners compose almost additively-discounted (+4.2/+3.7/+3.3/+2.1 solo → +7.2 together):
they attack different costs (flat addressing everywhere / present ALU / span store RMW / loop-exit
dispatch latency). One transient during verification ("only reached 1 frames" on an f40 shot) did
not reproduce — consecutive-process CUDA teardown overlap, not a code defect (f5/f40 re-runs exact).
