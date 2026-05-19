#!/bin/sh
# patch-xinitrc.sh — rewrite /root/.xinitrc inside rv64-disk.img (ext4).
# Run in WSL: bash patch-xinitrc.sh
#
# The --screenshot boot hard-kills the emulator, so the ext4 is left dirty
# with a populated journal. A debugfs edit on a dirty image is silently
# reverted by journal replay on the next mount — so e2fsck FIRST to replay
# and empty the journal, then debugfs writes survive.
set -e
IMG=/mnt/c/Users/Daniel/.cache/riscvemu/linux/rv64-disk.img
e2fsck -fy "$IMG" >/dev/null 2>&1 || true
cat > /tmp/new-xinitrc <<'EOF'
/usr/bin/rvemu-rv64-input &
xterm -fa "DejaVu Sans Mono" -fs 12 -geometry 92x28+40+40 &
exec icewm-session
EOF
debugfs -w -R "rm /root/.xinitrc" "$IMG" 2>/dev/null || true
debugfs -w -R "write /tmp/new-xinitrc /root/.xinitrc" "$IMG"
echo "=== patched /root/.xinitrc ($(wc -c < /tmp/new-xinitrc) bytes) ==="
debugfs -R "cat /root/.xinitrc" "$IMG" 2>/dev/null
