# ISA Change Playbook — Adding or Removing a CPU ISA Feature

> How to change which RISC-V extension/instruction the emulator supports, end
> to end, without leaving a guest binary that traps. Written from two real
> iterations: removing **DIV/REM** (→ Zmmul), then removing **MUL** (→ plain
> `rv32i`). Companion to `CLAUDE.md`, `AGENTS.md`, `Architecture.md`.

## The one mental model that matters

There is **one CPU**, shared by every piece of guest code: the Linux kernel,
uClibc, busybox, every package, the bare-metal examples, the test programs,
and the TinyCC JIT's *output*. The instant **any** of them executes an
instruction the CPU no longer implements, the CPU raises an
illegal-instruction trap.

Therefore an ISA change is **all-or-nothing and system-wide**. You cannot
"remove `mul` for the tests but keep it for Linux." You do one *feature* at a
time, but each feature change must reach **every** binary the emulator runs.

A second model: the C `*` / `/` / `%` operators never change — they are
language features. `-march` only decides *how the compiler lowers them*:
to a hardware instruction, or to a **libcall** (`__mulsi3`, `__divsi3`, …).
Removing an instruction = "force the libcall path everywhere, then delete the
instruction from the CPU."

## The hazard that will bite you: hand-emitted instructions

`-march` controls **only code the compiler generates from C**. It does **not**
touch instructions that are written literally. Three places hide them:

| Source of literal instructions | Retargetable by `-march`? | Where it bit us |
|---|---|---|
| Compiler-generated `*`/`/`/`%` | ✅ yes — emits libcall | normal C |
| Inline asm / `.S` files | ❌ no — literal | kernel atomics (A-ext) |
| A JIT code generator | ❌ no — emits raw encodings | `Examples/TinyCC/tinycc/riscv32-gen.c` |
| **Prebuilt binaries** copied in, not recompiled | ❌ no — already compiled | Microwindows `nano-X` etc. |

Every one of these must be found and fixed by hand.

---

## Step 0 — Decide the target ISA string

Pick the exact `-march` strings up front. Removing the M/F extensions entirely:
- Bare-metal: `rv32i`; floating-point C must lower to software libcalls.
- Linux with A still present: `rv32ia`. If removing A too, Linux becomes
  `rv32i` and needs kernel/libc atomic fallbacks because LR/SC/AMO are often
  hand-written assembly.

Sub-extensions matter: to keep multiply but drop divide, the ratified subset
is **`Zmmul`** → `rv32i_zmmul` / `rv32ia_zmmul`. Verify your clang (≥15) and
buildroot GCC (≥13) actually support the string before relying on it.

## Step 1 — The CPU (`Native/rv32i_core.cpp`)

The hot path is one `switch (opcode)` in `do_step`. For RV32 the M extension
is OP-class opcode `0x33` with `funct7 == 0x01`.

- To **remove**: in the relevant `case`, set `trap_cause = 2` (illegal
  instruction), `trap_tval = instr`, and `break` **without** writing `rd`.
  The existing `if (trap_cause) do_trap(...)` path takes over. Trapping
  loudly is the point — a stray instruction fails visibly instead of
  silently computing garbage.
- To **add**: implement it in the `case` and produce the result.
- Delete now-dead helper functions (`exec_mul` etc.).
- Update the ISA comment block at the top of the file and the table in
  `CLAUDE.md`.

Then rebuild and verify the build is clean:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  Native\rv32i_core.vcxproj -p:Platform=x64 -p:Configuration=Release
```

> Note: the `misa` CSR is decorative here — nothing gates on it and Linux
> reads `riscv,isa` from the DTB. Leaving it inaccurate is a conscious,
> low-risk choice; changing it risks a mismatch check in some kernels.

The test harness loads the **Debug** DLL (`Native/bin/Debug/rv32i_core.dll`,
copied by `Core/RiscVEmulator.Core.csproj`). A full solution build refreshes
it; if you build only the `vcxproj` in Release, the tests still see the old
Debug DLL. **This stale-DLL trap caused a completely false test reading once
— always confirm which DLL the tests actually load.**

## Step 2 — Bare-metal examples + tests

These compile C → RV32 ELF directly with clang. Change the `-march` in every
build invocation:

- `Examples/*/Program.cs` — grep for `march=`.
- `RiscVEmulator.Tests/EmulatorTestBase.cs` and `VoxelTest.cs`.
- Stale `-march` mentions in C-file header comments (cosmetic, fix anyway).

`Runtime/runtime.c` already provides software `__mulsi3` / `__divsi3` /
`__udivdi3` / … built from shift+add+sub only. Any bare-metal program that
switches to a reduced `-march` must **link `runtime.c`** so the libcalls
resolve — most already do.

## Step 3 — The TinyCC JIT (`Examples/TinyCC`)

TinyCC JIT-compiles C *at runtime*; its RV32 code generator
`tinycc/riscv32-gen.c` emits machine code that `-march` cannot touch.

- Audit `gen_opil` (and `gen_opl` in `tccgen.c` for 64-bit) for raw
  `ER(0x33, …, 1)` emissions of `mul`/`div`/`rem`.
- Reroute them through libcalls the way division already is: a helper that
  does `vpush_helper_func(TOK___mulsi3)` + `gfunc_call`.
- Add the `TOK___*` tokens to `tcctok.h` under the `TCC_TARGET_RISCV32` guard.
- Register the helpers with `tcc_add_symbol` in `Programs/tinycc_demo.c`.
- Change `Examples/TinyCC/Program.cs` `-march` too.

## Step 4 — The buildroot toolchain (`Examples/Linux.Build_RV32i/Program.cs`)

The internal cross-toolchain's default `-march` propagates automatically to
uClibc, busybox, **every package**, and the `bin/xc.sh` overlay binaries —
none of them pass an explicit `-march`.

- The `BR2_RISCV_ISA_RV*` knobs (RVI/RVM/RVA/…) are coarse. For a sub-set
  like Zmmul there is **no buildroot knob** — sed-patch `arch/arch.mk.riscv`,
  the one place the internal toolchain's `-march` is computed. (`Program.cs`
  section 6a.)
- libgcc is built **by buildroot as part of the toolchain**, so all normal
  userspace gets `__mulsi3`/`__divsi3` for free — you do **not** supply them.
- **A toolchain ISA change requires a full `make clean` rebuild.** Buildroot
  rebuilds a package only when its build *stamp* is invalidated; dir-cleaning
  just the toolchain leaves every already-built package (`doom-puredoom`,
  `ncurses`, `bc`, `nano`, …) shipping its old binary — which traps with
  `SIGILL` at runtime. A selective `*-dirclean` of only toolchain+kernel is
  **not enough**. The marker block in `Program.cs` does `make clean` (keeps
  `dl/` + `.config`) so toolchain, kernel and every package rebuild together.

## Step 5 — The Linux kernel

Two things, both via patches dropped in `board-patches/linux/` and applied by
`BR2_GLOBAL_PATCH_DIR`:

1. **The kernel sets its own `-march`** in `arch/riscv/Makefile`
   (`riscv-march-y`) — it does *not* inherit the toolchain default. Patch it
   (`0001-*.patch`).
2. **The kernel links no libgcc.** It has only `arch/riscv/lib/`. Drop M and
   the kernel needs `__mulsi3`/`__muldi3`/`__divsi3`/`__udivdi3`/… that
   aren't there. Add them as a patch (`0002-*.patch` → `mul32.c` + `div32.c`),
   implemented with **shift+add/sub only** (no `mul`/`div` — they'd trap),
   each `EXPORT_SYMBOL`'d, with `arch/riscv/lib/Makefile` entries.

`Program.cs` forces the one-time toolchain+kernel rebuild with a marker file
(`board/rvemu/.rv32i-noa-applied` for the no-A/no-M milestone): if absent, `make clean` + rebuild, then
`touch` the marker. Bump the marker name on every ISA change so an old tree
rebuilds exactly once.

**Kernel-hash gotcha:** a forced `linux-dirclean` makes buildroot re-verify
the kernel tarball. If `linux/linux.hash` has no entry for the pinned version
(2024.05.3 ships `6.6.44`, the config pins `6.6.18`), the download step
aborts with `No hash found`. `Program.cs` section 6a-ter appends the upstream
sha256 to fix this.

## Step 6 — Prebuilt guest binaries (Microwindows)

`Program.cs` **copies** the Microwindows binaries from
`~/rvemu-mw/microwindows/src/bin/` into the rootfs — it never rebuilds them.
After a toolchain ISA change they are stale and crash with `SIGILL`.

Rebuild in place against the new toolchain:

```bash
cd ~/rvemu-mw/microwindows/src && make clean && make -j$(nproc)
```

Do **not** rerun `scripts/build-microwindows.sh` for this — it does
`git checkout -- src/demos/nanox/Makefile`, dropping the `rvemu-taskbar` /
`rvemu-term` targets. The in-place rebuild keeps every customization.

Anything else prebuilt-and-copied (not built by buildroot) needs the same
treatment.

## Step 7 — Rebuild the image and boot

```powershell
dotnet run --no-build --project Examples\Linux.Build_RV32i -p:Platform=x64 -- --skip-apt
dotnet run --no-build --project Examples\Linux              -p:Platform=x64
```

A full toolchain+kernel rebuild is ~20–40 min; an overlay-only restage is
~5–10 min.

## Step 8 — Verify

1. **objdump every binary** for the removed mnemonics. Use the mnemonic
   column only — `<__divsi3>`/`<div>` symbol *labels* are false positives:
   ```bash
   llvm-objdump -d file.elf | grep -E $'\t(mul|mulh|mulhsu|mulhu|div|divu|rem|remu)\b'
   ```
   Check: bare-metal ELFs, kernel `vmlinux`, rootfs binaries, Microwindows
   binaries.
2. **Boot Linux** — it must reach the `~ #` shell with no
   `unhandled signal 4` / `cause: 00000002`. Decode any `badaddr` as an
   instruction encoding to find what slipped through.
3. **TinyCC demo** must print `SUCCESS`.
4. Bare-metal examples run.

## Decoding a trap

`cause: 0x2` = illegal instruction. `badaddr` (a.k.a. `mtval`) holds the
**instruction encoding**. Decode RV32: bits[6:0]=opcode, [14:12]=funct3,
[31:25]=funct7. `02d70733` → opcode `0x33`, funct7 `0x01`, funct3 `0` =
`mul`. That immediately tells you a binary still has the instruction → it
wasn't recompiled.

## Checklist

- [ ] CPU `do_step` updated; dead helpers deleted; native DLL rebuilt
- [ ] `CLAUDE.md` + `rv32i_core.cpp` header ISA tables updated
- [ ] Bare-metal `Program.cs` + test `-march` flags changed
- [ ] `runtime.c` provides the needed libcalls (or they're added)
- [ ] TinyCC `riscv32-gen.c` / `tccgen.c` rerouted; tokens + symbols added
- [ ] Buildroot toolchain `-march` patched (`arch.mk.riscv`)
- [ ] Kernel `arch/riscv/Makefile` march patch
- [ ] Kernel lib routines patch (kernel has no libgcc)
- [ ] Rebuild marker name bumped
- [ ] Microwindows (and any other prebuilt guest binaries) rebuilt
- [ ] objdump verification clean across **all** binary classes
- [ ] Linux boots to shell; TinyCC demo `SUCCESS`; examples run

## Known pitfalls (each cost real debugging time)

- **Stale native DLL** — tests/examples silently load the old DLL if only
  one config was rebuilt. Confirm the loaded DLL's timestamp.
- **Stale cached ELFs** — the test harness leaves `.elf`/`.o` in `bin/`;
  clear them (`find … -name '*.elf' -o -name '*.o' | xargs rm`) before
  trusting a run.
- **Pre-existing broken tests** — `RiscVEmulator.Tests` fails ~125/169 on a
  *pristine* `HEAD` checkout, unrelated to any ISA work. Always bisect a
  suspicious failure against pristine `HEAD` before blaming your change.
- **`misa` lies** — it is not kept in sync and nothing reads it; don't use it
  to verify.
- **Prebuilt ≠ rebuilt** — anything copied into the rootfs rather than
  compiled by buildroot is invisible to a toolchain change.
- **Stale package build-stamps** — buildroot does not rebuild a package
  just because the toolchain changed under it. After an ISA change, every
  package built before the toolchain still has M opcodes. Diagnose by
  comparing `output/build/<pkg>/.stamp_built` mtimes against the toolchain's
  (`host-gcc-final-*/.stamp_built`); cure with a full `make clean`.
- **Keep the kernel version string honest** — `Examples/Linux.Build_RV32i`
  rewrites the upstream template's `CONFIG_LOCALVERSION="mini-rv32ima"` to
  `-rv32i`. If the boot banner still says `mini-rv32ima`, you are booting a
  stale cached image or the legacy `--download` image. Still verify the kernel
  with objdump of `vmlinux`; the banner is only a label.
