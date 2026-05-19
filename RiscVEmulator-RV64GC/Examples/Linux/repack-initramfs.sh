#!/bin/sh
# repack-initramfs.sh — rebuild initramfs-rv64.cpio with the current
# install-xfce.sh from the repo. Run inside WSL: bash repack-initramfs.sh
# Idempotent: always extracts from the pristine .bak.
set -e
D=/mnt/c/Users/Daniel/.cache/riscvemu/linux
SRC=/mnt/c/work/RiscV/RiscVEmulatorMMU/Examples/Linux
[ -f "$D/initramfs-rv64.cpio.bak" ] || cp "$D/initramfs-rv64.cpio" "$D/initramfs-rv64.cpio.bak"
rm -rf /tmp/irfs && mkdir /tmp/irfs && cd /tmp/irfs
cpio -idm < "$D/initramfs-rv64.cpio.bak" 2>/dev/null
cp "$SRC/install-xfce.sh" ./install-xfce.sh
sed -i 's/\r$//' ./install-xfce.sh
# the audio bridge daemon, baked in so install-xfce.sh can copy it to the disk
[ -f "$SRC/rvemu-rv64-audio" ] && cp "$SRC/rvemu-rv64-audio" ./rvemu-rv64-audio
chmod +x ./install-xfce.sh ./init ./rvemu-rv64-audio 2>/dev/null
find . | cpio -o -H newc 2>/dev/null > "$D/initramfs-rv64.cpio"
echo REPACKED
ls -l "$D/initramfs-rv64.cpio"
echo -n "icewm lines in baked install script: "
grep -c icewm ./install-xfce.sh
