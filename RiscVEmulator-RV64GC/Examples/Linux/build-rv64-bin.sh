#!/bin/sh
# build-rv64-bin.sh — compile a self-contained bare-metal RV64GC C program
# to a flat binary runnable by `Examples.Linux --rv64 --rv64-bin <file>`.
# Usage: build-rv64-bin.sh <source.c>   (output: <source>.bin beside it)
set -e
BIN="$HOME/rvemu-buildroot/output-rv64/host/bin"
GCC="$BIN/riscv64-buildroot-linux-gnu-gcc"
OBJCOPY="$BIN/riscv64-buildroot-linux-gnu-objcopy"
SRC="$1"
DIR=$(dirname "$SRC")
BASE=$(basename "$SRC" .c)
LD="$DIR/rv64-flat.ld"
"$GCC" -march=rv64imafdc -mabi=lp64d -mcmodel=medany -nostdlib -ffreestanding \
       -fno-pic -no-pie -static -O1 -Wl,-T,"$LD" "$SRC" -o "$DIR/$BASE.elf"
"$OBJCOPY" -O binary "$DIR/$BASE.elf" "$DIR/$BASE.bin"
ls -l "$DIR/$BASE.bin"
echo "BUILD_OK $DIR/$BASE.bin"
