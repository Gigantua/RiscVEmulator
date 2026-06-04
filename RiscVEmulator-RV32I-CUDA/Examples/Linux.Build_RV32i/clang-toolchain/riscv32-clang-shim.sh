#!/bin/bash
# riscv32-clang-shim — drop-in C compiler for the buildroot RV32 nommu build,
# backed by clang/lld instead of buildroot's own GCC.
#
# Reuses the buildroot GCC toolchain's sysroot (headers + uClibc-ng) and its
# libgcc.a + crt objects; only the *compiler* is swapped to clang. See
# CLANG_UNIFICATION_PLAN.md.
#
# Verified (stage 1): this flag set compiles+links a uClibc hello-world to a
# valid pure-rv32i ELF (Flags 0x0).
#
# Install: copy over output/host/bin/riscv32-buildroot-linux-uclibc-{gcc,cc}.

set -e

# ── Locate the buildroot GCC tree relative to this script ────────────────
# Script installed at $HOST/bin/<name>;  $HOST = output/host.
SELF=$(readlink -f "$0")
HOST=$(dirname "$(dirname "$SELF")")
TRIPLE=riscv32-buildroot-linux-uclibc
SYSROOT="$HOST/$TRIPLE/sysroot"

# GCC lib dir holds crtbegin*.o / crtend*.o / libgcc.a — version-globbed.
GCCLIB=$(echo "$HOST"/lib/gcc/$TRIPLE/*/ | tr -d ' ')
GCCLIB=${GCCLIB%/}

# clang's --gcc-toolchain auto-detection fails on the 'buildroot' vendor
# triple, so pass crt/libgcc paths explicitly via -B/-L instead.
# clang links -lgcc_eh for the static unwinder; buildroot's uClibc GCC ships
# only libgcc.a (unwinder objects included) — alias it once.
[ -e "$GCCLIB/libgcc_eh.a" ] || ln -sf libgcc.a "$GCCLIB/libgcc_eh.a"

# With -fno-integrated-as, clang invokes `as` from the -B search path. Make
# `as` (and `ld`, for paranoia) under -B resolve to the buildroot cross
# binutils instead of the host x86 ones — otherwise `/usr/bin/as` is picked
# and rejects -mabi etc.
[ -e "$GCCLIB/as" ] || ln -sf "$HOST/bin/$TRIPLE-as" "$GCCLIB/as"
[ -e "$GCCLIB/ld" ] || ln -sf "$HOST/bin/$TRIPLE-ld" "$GCCLIB/ld"

# Pick an lld.
for c in ld.lld ld.lld-18 ld.lld-20; do
    command -v "$c" >/dev/null 2>&1 && { LLD=$(command -v "$c"); break; }
done

# ── Translate / drop gcc-only flags clang rejects outright ──────────────
ARGS=()
for a in "$@"; do
    case "$a" in
        -dN) ARGS+=("-dM") ;;        # uClibc gen_bits_syscall_h.sh: clang has no -dN
        -mtune=*|-fno-tree-*|-fno-ipa-*|-fno-reorder-functions| \
        -fconserve-stack|-fno-var-tracking-assignments) ;;     # dropped
        -march=*|-mabi=*) ;;                                   # ours wins
        *) ARGS+=("$a") ;;
    esac
done

exec clang \
    --target="$TRIPLE" \
    -march=rv32i -mabi=ilp32 -mno-relax \
    --sysroot="$SYSROOT" \
    -B"$GCCLIB" -L"$GCCLIB" \
    -B"$HOST/bin" \
    -fno-integrated-as \
    --ld-path="$LLD" \
    --rtlib=libgcc \
    -Qunused-arguments \
    -Wno-unknown-warning-option \
    -Wno-error=implicit-function-declaration \
    -Wno-error=implicit-int \
    -Wno-error=int-conversion \
    "${ARGS[@]}"
