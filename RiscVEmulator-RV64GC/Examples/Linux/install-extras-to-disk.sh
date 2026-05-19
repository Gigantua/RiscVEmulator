#!/bin/sh
# install-extras-to-disk.sh — debugfs-install the audio daemon binary and the
# setup-extras.sh script into rv64-disk.img. Run in WSL. e2fsck first so the
# writes survive the next boot's journal replay.
set -e
IMG=/mnt/c/Users/Daniel/.cache/riscvemu/linux/rv64-disk.img
E=/mnt/c/work/RiscV/RiscVEmulatorMMU/Examples/Linux
e2fsck -fy "$IMG" >/dev/null 2>&1 || true

# audio bridge daemon (binary — no sed!)
debugfs -w -R "rm /usr/bin/rvemu-rv64-audio" "$IMG" 2>/dev/null || true
debugfs -w -R "write $E/rvemu-rv64-audio /usr/bin/rvemu-rv64-audio" "$IMG"
debugfs -w -R "sif /usr/bin/rvemu-rv64-audio mode 0100755" "$IMG"

# guest setup script (text — strip CR)
sed 's/\r$//' "$E/setup-extras.sh" > /tmp/setup-extras.sh
debugfs -w -R "rm /root/setup-extras.sh" "$IMG" 2>/dev/null || true
debugfs -w -R "write /tmp/setup-extras.sh /root/setup-extras.sh" "$IMG"

echo INSTALL_OK
debugfs -R "stat /usr/bin/rvemu-rv64-audio" "$IMG" 2>/dev/null | grep -E "Mode|Size:"
