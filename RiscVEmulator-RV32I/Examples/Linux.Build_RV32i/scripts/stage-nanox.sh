#!/bin/bash
# Copy Microwindows nano-X client lib + headers into buildroot's sysroot
# so packages built by buildroot (doomgeneric, future nano-X clients)
# can link against -lnano-X and include <microwin/nano-X.h>.
set -e
MW="$HOME/rvemu-mw/microwindows"
SYS="$HOME/rvemu-buildroot/output/host/riscv32-buildroot-linux-uclibc/sysroot"

if [ ! -f "$MW/src/lib/libnano-X.a" ]; then
    echo "stage-nanox: skipping — $MW/src/lib/libnano-X.a not found (build Microwindows first)"
    exit 0
fi
install -D -m644 "$MW/src/lib/libnano-X.a" "$SYS/usr/lib/libnano-X.a"
install -d "$SYS/usr/include/microwin"
cp "$MW/src/include/nano-X.h"   "$SYS/usr/include/microwin/"
cp "$MW/src/include/mwtypes.h"  "$SYS/usr/include/microwin/"
cp "$MW/src/include/mwconfig.h" "$SYS/usr/include/microwin/"
cp "$MW/src/include/nxcolors.h" "$SYS/usr/include/microwin/"
echo "stage-nanox: installed $(ls $SYS/usr/include/microwin/ | wc -l) headers + libnano-X.a"
