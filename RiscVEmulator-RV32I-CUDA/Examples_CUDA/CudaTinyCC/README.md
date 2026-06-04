# Examples_CUDA/CudaTinyCC — RV32I core running on the GPU (CUDA)

Proof that the RV32I CPU runs **on the GPU**: the same `do_step` interpreter as
`Native/rv32i_core.cpp`, ported to CUDA (`Native/rv32i_cuda.cu`) and executed in
a single GPU thread (`__launch_bounds__(1,1)`). The guest is identical to
`Examples/TinyCC` — a ~410 KB TinyCC ELF that JIT-compiles `fib` / `count_primes`
/ `mandelbrot` at runtime — so this proves a C compiler running inside a CPU
emulator that is itself running on a GPU.

## Run

```powershell
# Build the solution (builds rv32i_cuda.dll via nvcc + all C# projects)
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" RiscVEmulator.sln -p:Platform=x64

# Run (clang must be on PATH to cross-compile the guest ELF)
dotnet Examples_CUDA\CudaTinyCC\bin\x64\Release\net10.0\Examples.CudaTinyCC.dll
```

Expected: the fib sequence, `Primes up to 100: 25`, `up to 1000: 168`, a
Mandelbrot ASCII render, `SUCCESS: all JIT results correct!`, exit code 0 —
byte-for-byte the same guest output as the CPU `Examples/TinyCC`.

A single GPU thread runs at ~2–3 MIPS (no latency hiding, register file lives in
global memory), so the run takes a few seconds. `<<<1,1>>>` proves portability,
not speed; throughput will come from running many independent cores in parallel.

## How it works (vs the CPU emulator)

| Aspect | CPU (`rv32i_core.cpp`) | GPU (`rv32i_cuda.cu`) |
|---|---|---|
| ISA core | `cpu_step` / trap unit | **same code**, `__device__`, threaded on a per-core `CoreState` |
| Decoders | `constexpr` | `constexpr`, called from device via `--expt-relaxed-constexpr` |
| Memory seam | `*(volatile T*)(mem+addr)` | `mem_read/mem_write` do `if (range)` dispatch (RAM / trap page / MMIO) |
| MMIO | guarded pages → Windows VEH → `IPeripheral` | range check → device-side semantics against a managed `Periph` page |
| Buffers | one host VA reservation | per-core CUDA **managed** memory (`cudaMallocManaged`) |
| Run loop | `rv32i_step_n` | **batch-synchronous**: launch budget steps → `cudaDeviceSynchronize` → host reconciles |

### Why batch-synchronous
Windows/WDDM forbids the host from touching managed memory while a kernel runs.
So `CudaEmulator.StepN(n)` launches the kernel for `n` steps, synchronises, then
drains the UART TX ring and reads halt/exit — the host only ever touches managed
memory *between* launches. Each launch stays well under the ~2 s WDDM TDR
watchdog.

### MMIO without a VEH
`mem_read`/`mem_write` (in `rv32i_cuda.cu`) branch on the guest address:
RAM and the trap page (`0x0F000000`) are plain managed memory; UART
(`0x10000000`) and host-exit (`0x40000000`) get simple device-side semantics
(TX ring push, RX pop, exit-code latch). Unknown ranges read 0 / ignore writes,
mirroring the CPU VEH's "no device" behaviour.

## Multi-core ready (future)
All state is per-core, indexed by `coreId` (`CoreState[]` / `CoreMem[]` in the
DLL). Today `nCores = 1` (`<<<1,1>>>`); raising it and the grid launches N
independent RV32I guests in one kernel. A frontend would then select which
core's console/framebuffer/audio to view.

## Peripherals (done)
Keyboard, mouse, RTC, MIDI, audio (PCM + control), and framebuffer + display
control are all supported on the CUDA backend via the same managed-page +
device-side `mmio_*` + between-batch reconcile pattern. `CudaEmulator` stages
input (keyboard/mouse/time) into the managed peripheral page **before** each
launch and drains output (UART/MIDI/audio/framebuffer) **after**, reusing the
existing `Core/Peripherals` device objects as the SDL/console-facing endpoints.
SDL only ever reads host-side snapshots (`FramebufferDevice.PresentedPixels`, a
mirrored audio buffer) — never managed memory while a kernel runs.

- **`Examples_CUDA/CudaPeriphTest`** — headless self-test: a freestanding guest
  exercises every peripheral via MMIO; the host pre-feeds keyboard/mouse and
  asserts the read-back (17 checks, incl. both framebuffer present paths — direct
  and `fbaddr`→RAM; no SDL, no audible playback). Run:
  `dotnet Examples_CUDA\CudaPeriphTest\bin\x64\Release\net10.0\Examples.CudaPeriphTest.dll`
- **`Examples_CUDA/CudaGfx`** — a tiny multiply-free guest animates a plasma
  straight to the framebuffer; renders visibly fast through `Frontend/SdlWindow`
  on the GPU core. `--selftest` asserts the FB renders **and animates** headless.
  This is the quickest way to *see* the framebuffer working on CUDA.
- **`Examples_CUDA/CudaVoxel`** — the full Voxel terrain renderer through
  `SdlWindow` (driven via the `IEmulator` seam — `Emulator` and `CudaEmulator`
  both implement it). It's softfloat-heavy, so world generation + first frame
  take a long time on a single GPU thread (it stays black during world-gen —
  that's speed, not a framebuffer bug; `--headless` reports when it renders).

## Networking (interrupt pin plumbed; virtio pending Linux-on-CUDA)
The external-interrupt pin is wired (`cuda_rv32i_set_meip` →
`CudaEmulator.SetMachineExtIrq`), and guest RAM is host-accessible between
launches (managed memory) — which is exactly what virtio ring DMA needs.
However, **no bare-metal example uses networking**; virtio-net is only
exercised by the Linux guest, and Linux-on-CUDA (96 MB RAM, CLINT/PLIC timing,
the full boot) is the remaining large milestone. Completing networking means:
port the virtio-mmio register state machine to device code (the init handshake
is synchronous within a batch), run the descriptor-ring DMA host-side between
batches over the managed guest RAM (reusing `Core/Peripherals/VirtioNetDevice`
and `Core/Networking/*`), and drive MEIP from the PLIC. The plumbing is in
place; the consumer (a networked guest) is what's missing.

## Build note
`Native/rv32i_cuda.vcxproj` is an **NMake** project that invokes nvcc directly
(`-arch=sm_86 --expt-relaxed-constexpr -cudart static`) through `vcvars64`. It
does *not* use the VS "CUDA NN.N" build customization because that passes an
empty `-ccbin` under this VS18 toolchain. The equivalent dev command:

```
nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 -shared -cudart static ^
     -o Native\bin\Release\rv32i_cuda.dll Native\rv32i_cuda.cu
```
