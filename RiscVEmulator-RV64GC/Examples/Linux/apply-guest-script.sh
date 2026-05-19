#!/bin/sh
# apply-guest-script.sh — install a host script file into rv64-disk.img via
# debugfs. Run in WSL: bash apply-guest-script.sh <host-script> <guest-path>
# e2fsck FIRST — the --screenshot boot leaves ext4 dirty and journal replay
# would otherwise revert the write.
set -e
IMG=/mnt/c/Users/Daniel/.cache/riscvemu/linux/rv64-disk.img
SRC="$1"
DEST="$2"
e2fsck -fy "$IMG" >/dev/null 2>&1 || true
sed -i 's/\r$//' "$SRC"
debugfs -w -R "rm $DEST" "$IMG" 2>/dev/null || true
debugfs -w -R "write $SRC $DEST" "$IMG"
echo "=== installed $DEST ($(wc -c < "$SRC") bytes) ==="
debugfs -R "cat $DEST" "$IMG" 2>/dev/null | head -4
