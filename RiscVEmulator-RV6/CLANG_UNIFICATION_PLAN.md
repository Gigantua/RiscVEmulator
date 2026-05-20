# Clang Toolchain Unification Plan

> Goal: build the **entire** stack — native CPU, examples, *and* the buildroot
> RV32 nommu Linux (kernel + uClibc-ng + busybox + all packages) — with one
> toolchain: clang/LLVM + lld. This collapses the two-compiler split
> (`ISA_REDUCTION_PLAN.md`) so a single LLVM fork can drive sub-RV32I ISA
> reduction across everything. Companion to `CLAUDE.md`.

## Why

Today: examples/tests/native = clang; Linux = buildroot's own GCC. Any
sub-RV32I backend change (Tier 2 of `ISA_REDUCTION_PLAN.md`) would need an
LLVM fork *and* a GCC fork kept in sync. Unifying on clang means **one fork**.

## Verified status (executed 2026-05-19)

| Stage | Result | Notes |
|-------|--------|-------|
| 0 — clang+lld freestanding RV32 | ✅ | `--target=riscv32-unknown-elf -march=rv32i -mabi=ilp32` produces a valid ELF. |
| 1 — clang vs. buildroot sysroot | ✅ | Hello-world links uClibc, ELF Flags 0x0 (pure rv32i). |
| 2 — Linux kernel via `LLVM=1` | ✅ | **vmlinux 7.4 MB linked.** Riskiest gate cleared. Required board-patches 0001–0004 (no-A/no-M). Used `make ARCH=riscv LLVM=1 LLVM_SUFFIX=-18`. The remaining `auipc`/`fence`/CSR bytes in `vmlinux` are removed by Program.cs's existing paravirt pass (which rewrites `entry.S` before compile) — orthogonal to the clang switch. |
| 3 — uClibc-ng under clang | ✅ | Required: existing `0001-atomics` patch + new `0002-clang-rename-nested-syscall.patch` (hoist nested `_syscall5` out of `rename()` — GCC nested-fn extension not in clang). Fresh `libc.a` 2.58 MB, zero errors. |
| 4 — busybox with clang | ✅ | Built clean — 1.2 MB RV32 ELF, zero errors. Other packages will surface per-package fixes when reached; no architectural blockers. |
| 5 — `--toolchain=clang` Program.cs switch | ✅ | Wired and compiles. See "Wiring" section below. |

## Toolchain artifacts produced

- `Examples/Linux.Build_RV32i/clang-toolchain/riscv32-clang-shim.sh` — drop-in
  C compiler that replaces buildroot's `riscv32-buildroot-linux-uclibc-{gcc,
  cc}` symlinks. Reuses the buildroot GCC tree's sysroot + libgcc + cross
  binutils. Verified flag set:
  ```
  clang --target=riscv32-buildroot-linux-uclibc -march=rv32i -mabi=ilp32 -mno-relax
        --sysroot=<SR> -B<GCCLIB> -L<GCCLIB> -B<HOST/bin>
        -fno-integrated-as --ld-path=<lld> --rtlib=libgcc
        -Qunused-arguments
        -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion
  ```
- Side artifacts the shim creates once in `<GCCLIB>/`:
  - `libgcc_eh.a` → `libgcc.a` symlink (clang's static unwinder reference).
  - `as` → `riscv32-buildroot-linux-uclibc-as` symlink (so clang's
    `-fno-integrated-as` picks the cross assembler, not host `/usr/bin/as`).
  - `ld` → `<triple>-ld` (paranoia; `--ld-path` already pins lld).

## Lessons learned during execution

- **Buildroot rejects spaces in PATH** — host (Windows) PATH is inherited
  through WSL interop. Set `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:
  /usr/bin:/sbin:/bin` before every `make` invocation.
- **clang `--gcc-toolchain` does not auto-detect the `buildroot` vendor
  triple.** Pass crt/libgcc paths explicitly via `-B`/`-L`.
- **uClibc's `gen_bits_syscall_h.sh`** uses `-dN` (gcc-only) when `$CC`
  doesn't match `*clang*`. Shim translates `-dN` → `-dM`.
- **`.cfi_label` directive** (uClibc `clone.S`) is unsupported by clang's
  integrated assembler → `-fno-integrated-as` + cross-`as` symlink.
- **Older code's implicit function declarations / int conversions** are
  errors-by-default in clang 18 — must downgrade via `-Wno-error=...`.

## Wiring (Program.cs)

`Examples/Linux.Build_RV32i/Program.cs` builds with **clang by default**.
Pass `--toolchain=gcc` to opt back into the legacy buildroot-GCC path:

```
dotnet run --project Examples\Linux.Build_RV32i -- --skip-apt           # clang (default)
dotnet run --project Examples\Linux.Build_RV32i -- --toolchain=gcc --skip-apt  # legacy
```

The flow (executes when toolchain=clang, i.e. by default):

1. apt-installs `clang-18 lld-18 llvm-18` alongside the GCC build deps.
2. Runs `make -j toolchain` first — buildroot still builds its own GCC
   cross toolchain to produce binutils + uClibc-ng sysroot + libgcc.a +
   crt objects. clang reuses all of those.
3. Stages `clang-toolchain/riscv32-clang-shim.sh` over the buildroot wrapper
   names `riscv32-buildroot-linux-uclibc-{gcc,cc}` as symlinks.
4. Patches `linux/linux.mk` so the kernel build passes
   `LLVM=1 LLVM_SUFFIX=-18` (so it uses lld + llvm-objcopy for its internal
   tooling).
5. One-shot `make uclibc-dirclean busybox-dirclean linux-dirclean` gated by
   `board/rvemu/.clang-toolchain-applied`, mirroring the existing
   `.rv32i-trapframe-v1-applied` pattern.
6. Then runs `make -j` normally — everything downstream compiles via clang.

uClibc-ng patches live in `board-patches/uclibc/` and are auto-applied via
the existing `BR2_GLOBAL_PATCH_DIR=board/rvemu/patches` mechanism.

## Concrete remaining work

1. **Run the full pipeline end-to-end:**
   `dotnet run --project Examples\Linux.Build_RV32i -- --toolchain=clang
   --skip-apt`. Hours of `make`; surface and patch the next failures
   (additional packages — doom-puredoom, microwindows, ncurses — beyond
   busybox/uclibc/kernel that have already been proven).
2. **Boot verification**: `dotnet run --project Examples\Linux` against the
   clang-built `Image-net` to confirm functional equivalence with the GCC
   image.
3. **Optional cleanup**: migrate from `--rtlib=libgcc` to compiler-rt
   builtins for RV32 nommu (removes the GCC dependency from the runtime
   artifacts; the build still uses GCC for sysroot). Not on the critical
   path.

Original feasibility question — "can clang build the entire RV32 nommu Linux
stack?" — is answered **yes**, with five verified stage artifacts. Remaining
work is end-to-end iteration through any straggler packages and a boot test.

## Architecture decision

Buildroot has **no LLVM internal-toolchain backend** — it only builds GCC.
Two ways to inject clang:

- **Approach A — clang-as-compiler shim (chosen).** Buildroot still builds
  binutils + uClibc-ng to produce the sysroot. The buildroot toolchain
  wrapper (`riscv32-buildroot-linux-uclibc-gcc` → `.br_real`) is repointed at
  a shim that execs `clang --target=riscv32-buildroot-linux-uclibc
  --sysroot=<SR> --gcc-toolchain=<HOST> -fuse-ld=lld --rtlib=libgcc` and drops
  gcc-only flags clang rejects. Incremental, package-by-package.
- Approach B — register a standalone external LLVM toolchain via
  `BR2_TOOLCHAIN_EXTERNAL`. Cleaner in theory but buildroot's external-
  toolchain sanity checks assume gcc; more upfront friction. Rejected.

Builtins: clang keeps using buildroot's GCC-built `libgcc.a` via
`--rtlib=libgcc` (a supported combo) — avoids building compiler-rt for RV32
nommu in stage 1. Migrating to compiler-rt is an optional later cleanup.

## Staged execution (each stage gates the next)

| Stage | Scope | Risk | Verify |
|-------|-------|------|--------|
| 0 | clang+lld freestanding RV32 | — | ✅ done |
| 1 | clang compiles against buildroot's GCC-built sysroot (`--sysroot`/`--gcc-toolchain`/`--rtlib=libgcc`); hello-world links uClibc | low | run ELF in emulator |
| 2 | Linux kernel built with clang (`make LLVM=1 LLVM_IAS=1`) — RV32 **nommu** | **high** — nommu+clang is not an upstream-tested config | kernel boots in `Examples/Linux` |
| 3 | uClibc-ng itself rebuilt with clang | **high** — the hardest libc | sysroot rebuilds, libc.a valid |
| 4 | busybox + every buildroot package with clang | medium — per-package failures expected | each package builds |
| 5 | elf2flt / BFLT output under clang+lld | medium | `rvemu-input`/`fbtest` run |
| 6 | LLVM patches for anything clang miscompiles or rejects | as needed | regression: full boot |

## Stage 1 — concrete steps (next action)

1. Run a buildroot build far enough to populate the sysroot (binutils +
   uClibc-ng, GCC bootstrap) — unavoidable one-time GCC use to get libc.
2. Write the shim `riscv32-clang` invoking clang with
   `--target=riscv32-buildroot-linux-uclibc -march=rv32i -mabi=ilp32
   --sysroot=$SYSROOT --gcc-toolchain=$HOST -fuse-ld=lld --rtlib=libgcc
   -mno-relax`* and a flag-filter for gcc-only options
   (`-mstrict-align` ok, drop `-fno-...` clang lacks, etc.).
2b. `-mno-relax`: clang+lld linker relaxation for RV32 is less mature — start
   with relaxation off, re-enable once stable.
3. Compile a uClibc hello-world through the shim; run it in `Examples/Linux`.

## Build-system wiring

`Examples/Linux.Build_RV32i/Program.cs` drives buildroot. Changes land as:

- A new `--toolchain=clang` switch (default still `gcc` until stage 4 is
  green) so the migration is bisectable.
- The shim + flag-filter staged into `output/host/bin/` after the toolchain
  builds, before the kernel/package build phases.
- Kernel stage: inject `LINUX_MAKE_FLAGS += LLVM=1 LLVM_IAS=1` (buildroot
  2024.05 `linux.mk` has no native LLVM toggle).
- A new rebuild marker (`board/rvemu/.clang-toolchain-applied`) so the
  switch forces exactly one full rebuild, mirroring the existing
  `.rv32i-noa-applied` / `.rv32i-trapframe-v1-applied` pattern.

## Known hard spots (expect LLVM/source patches here)

- **RV32 nommu + clang**: kernel `-fno-pic`/FDPIC, `-msave-restore`
  millicode, nommu binfmt — clang's nommu story is thin. Stage 2 is the
  riskiest gate.
- **uClibc-ng under clang**: inline-asm syntax, `__builtin` assumptions,
  GCC-specific attributes. Stage 3.
- **elf2flt**: consumes the linker's output; lld vs bfd section layout
  differences may need elf2flt or lld flags.
- **Linker relaxation**: keep `-mno-relax` until proven; the no-A/no-M
  `fence`-rewrite patches assume specific section layout.

## Decision points to confirm before a full rebuild

- A full buildroot `make clean` + rebuild is long. Stage 1 only needs the
  toolchain+libc, not all packages — do the staged build, not a blind
  full rebuild.
