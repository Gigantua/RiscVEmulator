#!/bin/bash
# build-microwindows.sh — one-shot setup + build of Microwindows nano-X for
# the rvemu nommu RV32I guest.
#
# Run after the buildroot toolchain at ~/rvemu-buildroot/output/host is built
# (the first emulator boot does that automatically). Produces BFLT binaries
# under ~/rvemu-mw/microwindows/src/bin/ which Examples/Linux.Build_RV32i
# auto-detects on next Prepare.
#
# Patched files (kept beside this script under microwindows/):
#   config       — minimal nommu-friendly config (no fonts/JPEG/etc, FB=RVEMU)
#   scr_rvemu.c  — custom screen driver, mmaps /dev/mem at 0x85FC0000
#
# Patches applied to the upstream tree:
#   * src/Arch.rules — add UCLINUX-RISCV target (BFLT linker flags)
#   * src/drivers/Objects.rules — register SCREEN=RVEMU
#   * src/include/mwconfig.h — AUTO_START_SERVER=0 (no fork() on nommu)
#   * src/demos/nanox/Makefile — strip fork-using demos + C++ targets
#   * src/demos/nanox/Makefile — enable nanowm target

set -e

MW_DIR="$HOME/rvemu-mw/microwindows"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="$SCRIPT_DIR/../microwindows"
BUILDROOT_HOST="$HOME/rvemu-buildroot/output/host"

if [ ! -x "$BUILDROOT_HOST/bin/riscv32-buildroot-linux-uclibc-gcc" ]; then
    echo "error: buildroot host toolchain not found at $BUILDROOT_HOST"
    echo "build the emulator and run Examples/Linux once so buildroot builds the toolchain."
    exit 1
fi

# 1. Clone if needed
if [ ! -d "$MW_DIR" ]; then
    echo "==> cloning microwindows"
    mkdir -p "$(dirname "$MW_DIR")"
    git clone --depth 1 https://github.com/ghaerr/microwindows.git "$MW_DIR"
fi

# 2. Install the rvemu-specific config + screen/keyboard drivers
echo "==> installing config + scr_rvemu.c + kbd_rvemu.c"
cp "$PATCH_DIR/config"        "$MW_DIR/src/config"
cp "$PATCH_DIR/scr_rvemu.c"   "$MW_DIR/src/drivers/scr_rvemu.c"
cp "$PATCH_DIR/kbd_rvemu.c"   "$MW_DIR/src/drivers/kbd_rvemu.c"

# 3. Patch Arch.rules — add UCLINUX-RISCV section
if ! grep -q "UCLINUX-RISCV" "$MW_DIR/src/Arch.rules"; then
    echo "==> patching Arch.rules"
    cat >> "$MW_DIR/src/Arch.rules" <<'EOF'

# rvemu RV32I nommu BFLT target (buildroot uclibc toolchain).
ifeq ($(ARCH), UCLINUX-RISCV)
TOOLSPREFIX = $(RISCVTOOLSPREFIX)
DEFINES += -DLINUX=1 -DUNIX=1 -DUCLINUX=1
CFLAGS += -fPIC
LDFLAGS += -Wl,-elf2flt=-r
FLTFLAGS += -s 64000
endif
EOF
fi

# 4. Patch Objects.rules — register SCREEN=RVEMU
python3 "$SCRIPT_DIR/inject-rvemu-screen.py"

# 5. Patch mwconfig.h — disable AUTO_START_SERVER (uses fork())
sed -i 's|^#define AUTO_START_SERVER.*|#define AUTO_START_SERVER   0   /* disabled: nommu has no fork */|' \
    "$MW_DIR/src/include/mwconfig.h"

# 6. Strip fork-using demos from demos/nanox/Makefile
( cd "$MW_DIR" && git checkout -- src/demos/nanox/Makefile 2>/dev/null ) || true
bash "$SCRIPT_DIR/patch-mw-makefile.sh"

# 7. Enable nanowm target
python3 "$SCRIPT_DIR/enable-nanowm.py"

# 8. Build
echo "==> building"
cd "$MW_DIR/src"
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
make clean >/dev/null 2>&1 || true
make -j"$(nproc)"

echo
echo "==> done. Binaries:"
ls -la "$MW_DIR/src/bin/nano-X" "$MW_DIR/src/bin/nanowm" \
       "$MW_DIR/src/bin/nxclock" "$MW_DIR/src/bin/nxeyes" 2>&1 | head -10

echo
echo "Re-run Examples/Linux.Build_RV32i to pick them up into the rootfs."
