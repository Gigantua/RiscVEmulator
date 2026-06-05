# CUDA RV32IMA perf — research batch 2 (CUDA 13.3 doc-grounded)

Context: the single-thread-per-core interpreter on sm_86. Batch-1 landed three
wins (commits U1/U12/U11): hoist CoreMem to a local copy, `__ldg` the fetch, and
serve the fetch from `__constant__` memory — together ~1.16 → ~3.7 MIPS single
core (≈3.2×) on an idle GPU. Key doc facts (CUDA 13.3, Tables 30/31):

- sm_86 RO/texture cache working set = 28–128 KB/SM, but **constant-cache working
  set is only 8 KB/SM** → `__ldg` scales to larger guests than `__constant__`
  (>8 KB thrashes the constant cache). The 8 KB bench guest fits both.
- sm_86: 1536 threads/SM, 48 warps, 16 blocks/SM, 64K regs/SM, 100 KB shared/SM,
  128 KB unified data cache. The 33 KB/block shared regfile (block=256) is the
  throughput-path occupancy limiter.
- L2 residency control (access-policy windows, cc 8.0+) is the doc-sanctioned fix
  for "a busy GPU thrashes L2 and collapses the __ldg win" (measured: 3.30 idle
  vs 1.32 under heavy background GPU load).

## 30 ideas (grouped)

A. Cache-operator loads/stores (doc 29): 1 `__ldca` fetch; 2 `__ldcs` streaming
   data loads; 3 `__ldlu` last-use; 4 `__stcs/__stwt` for fb/pcm stores;
   5 `__stcg` RAM stores.
B. L2 residency (doc 20): 6 persistence window on the code image; 7 on
   CoreState/CoreMem; 8 hitRatio + reset hygiene; 9 mark fb/pcm streaming.
C. L1/shared carveout (doc 04/10): 10 MaxL1 carveout on the latency kernel;
   11 round throughput shared to an exact SMEM bucket.
D. Occupancy (doc 25): 12 shrink regfile to ≤16 words when guest uses ≤16 regs;
   13 `__launch_bounds__(256, minBlocksPerSM)`; 14 reg-cap ≤42 regs/thread.
E. Warp cooperation (doc 29): 15 `__match_any_sync(pc)` group + shfl; 16
   `__ballot_sync(halted)` early warp exit; 17 broadcast decoded fields.
F. Compiler hints (doc 29): 18 `__builtin_assume` invariants; 19 assume in-bounds
   on JIT RAM fast path; 20 `[[likely]]/[[unlikely]]` hot/cold; 21 `__nv_pure__`
   decoders; 22 `#pragma unroll` A/B on the budget loop.
G. Register/addressing (doc 29): 23 32-bit shared address for the regfile base
   (`__cvta_generic_to_shared`); 24 same for the prefetch `sec` pointer.
H. ALU/decode (doc 29): 25 DPX `__vimin/__vimax` for AMO min/max + selects;
   26 `__byte_perm`/`__funnelshift` for J/B immediate bit-scatter.
I. Access shape (doc 04): 27 128-bit uint4 4-instruction fetch line; 28
   128-byte-align ram/code + sector-aligned code_lo.
J. Launch/API (doc 06/14): 29 CUDA Graph capture/replay of step_all;
   30 multi-stream concurrent core-groups.

## Tested so far (idle GPU; base compute 3.73 / data 3.85)

| Idea | Result | Verdict |
|---|---|---|
| 1  `__ldca` fetch        | 3.73 → ~3.2  | reverted (worse; `__ldg` RO cache wins) |
| 27 uint4 iline           | 3.73 → ~2.78 | reverted (worse; `iline[]` lands in local memory) |
| 11/occ MaxShared carveout| multi-core unchanged (noisy 4.8k–7.7k) | reverted (no measurable occupancy gain) |

## Conclusion

Single-core is now constant-cache-fetch-bound and the per-instruction chain is
dependent (no ILP), so device micro-opts test neutral-or-worse — consistent with
batch 1. The genuinely-untapped levers can't be judged by CudaBench wall-clock:
- **#6 L2 persistence** → only matters under GPU contention (validate by running a
  competing GPU load, not the idle bench). Worth landing defensively for the
  "while gaming" case.
- **#12–#14, #23 occupancy** → validate with `cudaOccupancyMaxActiveBlocksPerMultiprocessor`
  (deterministic), not noisy timing.
- Bigger single-core gains need the **JIT**, not these interpreter micro-opts.
