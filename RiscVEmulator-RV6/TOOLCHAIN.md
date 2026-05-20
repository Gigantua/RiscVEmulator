# Self-built LLVM/clang toolchain

The whole project — Windows-side Examples, .NET test suite, and the buildroot
RV32 nommu Linux kernel — compiles guest RV32I code with a **clang+lld we
built ourselves from source**. This document covers what got built, where it
lives, and how to use or rebuild it.

## What

LLVM 18.1.8, `release/18.x` branch, built with **ninja** (Release, no
asserts/tests/examples) and limited to two targets:

- **RISC-V** (riscv32 + riscv64) — emits the RV32I guest code.
- **X86** (i686 + x86-64) — host targets so clang can compile Linux ELFs
  for the WSL host (`build-llvm.sh` itself etc.).

Produced binaries: `clang` (`clang++`, `clang-cl`, …), `ld.lld`,
`llvm-objcopy`, `llvm-ar`, `llvm-ranlib`, `llvm-nm`, `llvm-strip`,
`llvm-objdump`, `llvm-readelf`. No other LLVM tools.

Bootstrapped off the system `clang-18` already shipped with the WSL Ubuntu
24.04 distro, so the build does *not* go through gcc. `-DLLVM_USE_LINKER=lld`
links via system lld during bootstrap; the final binaries are then used to
build everything else.

## Where it lives

Everything is **inside WSL** (Ubuntu-24.04 distro). Nothing is checked into
the repo — these are large generated binaries, ~1.5 GB installed.

| Path | Contents |
|------|----------|
| `~/llvm-project/` | Source tree, shallow clone of `release/18.x` (~2.1 GB) |
| `~/llvm-build/` | CMake/ninja build directory (~15 GB, can be deleted after install) |
| `~/llvm-rv32i/` | **Install prefix** — what the project uses. `~/llvm-rv32i/bin/clang` is the entry point. |

Override the install prefix anywhere with the `RVEMU_LLVM_BIN` env var
(see `Core/WslClang.cs`).

The only thing checked into the repo is the **build script**
[`build-llvm.sh`](./build-llvm.sh).

## How the project uses it

### Windows-side Examples + .NET tests

Every C# call to clang goes through [`Core/WslClang.cs`](./Core/WslClang.cs).
That helper:

1. Translates Windows path arguments (`C:\work\…\foo.c`, `-IC:\…`,
   `-Wl,-T,C:\…`, `--sysroot=C:\…`) into `/mnt/c/work/…/foo.c` form.
2. Invokes `wsl.exe -- ~/llvm-rv32i/bin/clang -B~/llvm-rv32i/bin <args>`.
   The `-B` adds the install dir to clang's tool-search path so
   `-fuse-ld=lld` finds the matching `ld.lld` next to it without any
   PATH munging.

Every Example (`Doom`, `Voxel`, `TinyCC`, `Sound`, `Video`, `Input`, `Midi`,
`Mp4Player`) and the test bases (`EmulatorTestBase`, `VoxelTest`) call
`WslClang.Run(args, out stderr)` instead of starting `clang.exe` directly.
There is no `C:\Program Files\LLVM\bin\clang.exe` reference in the repo.

### Linux kernel + buildroot stack

`Examples/Linux.Build_RV32i/Program.cs` builds the RV32 nommu Linux image
through buildroot. At startup it auto-detects `~/llvm-rv32i/bin/clang` and:

- Skips the apt-install of `clang-18 lld-18 llvm-18` (only installs them
  if the self-built tree is missing — first-run fallback).
- Prepends `~/llvm-rv32i/bin` to PATH for every WSL command, so the
  kernel's `make LLVM=1` invocation picks up unsuffixed `clang` / `ld.lld`
  / `llvm-objcopy` from the self-built tree.
- Drops `LLVM_SUFFIX=-18` from the kernel make flags (only used in the
  apt fallback).

The buildroot RV32 cross compiler itself is the **clang shim** at
[`Examples/Linux.Build_RV32i/clang-toolchain/riscv32-clang-shim.sh`](./Examples/Linux.Build_RV32i/clang-toolchain/riscv32-clang-shim.sh).
It's symlinked over `riscv32-buildroot-linux-uclibc-gcc` / `-cc` after the
buildroot GCC bootstrap finishes (buildroot still builds binutils +
uClibc-ng + libgcc via GCC; only the *compiler* is swapped to clang). The
shim execs `~/llvm-rv32i/bin/clang` and falls back to PATH clang if
unavailable. Override location via `RVEMU_LLVM_PREFIX`.

## How to (re)build it

One-time. From this repo, in WSL:

```bash
# 1. Clone (shallow, ~2 GB, ~5 min on a fast connection).
cd ~
git clone --depth 1 --branch release/18.x --single-branch \
    https://github.com/llvm/llvm-project.git

# 2. Build + install (~30-60 min on 32 cores; uses ~15 GB during build,
#    leaves ~1.5 GB installed). Script lives in this repo.
~/your/path/to/RiscVEmulator-RV6/build-llvm.sh

# 3. (Optional) reclaim the build directory.
rm -rf ~/llvm-build
```

`build-llvm.sh` is idempotent — it `rm -rf`s `~/llvm-build` first, then
reconfigures + builds + installs.

Verify:

```bash
~/llvm-rv32i/bin/clang --version                # should say 18.1.8
~/llvm-rv32i/bin/clang -print-targets | grep -i riscv
# Expect:  riscv32 - 32-bit RISC-V / riscv64 - 64-bit RISC-V

# Smoke test — should emit ELF32 RISC-V, Flags 0x0 (pure rv32i).
echo 'int _start(void){return 42;}' > /tmp/t.c
~/llvm-rv32i/bin/clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
    -nostdlib --ld-path=$HOME/llvm-rv32i/bin/ld.lld /tmp/t.c -o /tmp/t.elf
~/llvm-rv32i/bin/llvm-readelf -h /tmp/t.elf | grep -E 'Machine|Flags'
```

## What changes if you want a different LLVM

The wiring uses only one path (`~/llvm-rv32i/bin`). To swap LLVM versions
(e.g. main, release/19, a fork):

```bash
# In ~/llvm-project, switch branch and re-run.
git fetch origin <branch> --depth 1
git checkout FETCH_HEAD
~/your/path/to/RiscVEmulator-RV6/build-llvm.sh
```

The `riscv32-clang-shim.sh` and `WslClang.cs` paths don't need updating —
they reference the install prefix. Patched flags (`-Wno-error=...`, the
`-dN` → `-dM` translation, the `libgcc_s` symlink) target the
buildroot-GCC sysroot, not the clang tree, so they survive an LLVM upgrade.

## Why we built it ourselves

Original goal (`CLANG_UNIFICATION_PLAN.md` in `RiscVEmulator-RV32I/`): one
toolchain drives the entire stack so a single LLVM fork can carry sub-RV32I
ISA reduction patches across kernel + userspace + examples. Pinning to a
specific commit, applying local backend patches, and adding new opcodes
all require source access — `apt install clang-18` is too downstream.

Today the build is unmodified upstream `release/18.x`. The patch slot is
reserved.
