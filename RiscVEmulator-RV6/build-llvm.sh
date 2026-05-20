#!/usr/bin/env bash
# Configure + build LLVM 18 (clang + lld, X86 + RISCV targets) with ninja.
# Bootstraps off the system clang-18 we already have.
set -euo pipefail

SRC=$HOME/llvm-project
BUILD=$HOME/llvm-build
INSTALL=$HOME/llvm-rv32i

[ -d "$SRC/llvm" ] || { echo "missing $SRC/llvm — clone failed?"; exit 1; }

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# Notes:
#   * Bootstrap with system clang-18 → fast.
#   * Use system lld for the *bootstrap* link step (saves ~minutes).
#   * Disable assertions, tests, examples, benchmarks → smaller + faster.
#   * Parallel link jobs capped at 8 (link uses ~3GB RAM each).
#   * Skip building the static analyzer; clang-tools-extra; libc; etc.
cmake -G Ninja "$SRC/llvm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_USE_LINKER=lld \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD="X86;RISCV" \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DCLANG_ENABLE_ARCMT=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=8 \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    2>&1 | tail -40

echo
echo "=== configure done — starting ninja ==="
date

ninja clang lld llvm-objcopy llvm-ar llvm-ranlib llvm-nm llvm-strip llvm-objdump llvm-readelf

echo
echo "=== ninja done — installing ==="
date
ninja install

echo
echo "=== install done ==="
ls -la "$INSTALL/bin" | head -20
"$INSTALL/bin/clang" --version
echo
echo "=== RISCV targets in clang ==="
"$INSTALL/bin/clang" -print-targets | grep -i riscv || true
