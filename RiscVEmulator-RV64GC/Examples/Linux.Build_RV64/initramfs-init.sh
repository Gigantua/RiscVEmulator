#!/bin/sh
# rvemu RV64 initramfs init.
#  • If /dev/vda holds an installed Alpine system → switch_root into it
#    (the XFCE desktop boots from the persistent disk).
#  • Otherwise → drop to an initramfs shell (install / recovery mode; the
#    boot harness injects `sh /install-xfce.sh` here on first run).
mount -t proc     proc /proc 2>/dev/null
mount -t sysfs    sys  /sys  2>/dev/null
mount -t devtmpfs dev  /dev  2>/dev/null
mkdir -p /mnt

if mount -t ext4 -o ro /dev/vda /mnt 2>/dev/null; then
    if [ -x /mnt/sbin/init ]; then
        echo
        echo "rvemu RV64 — booting Alpine from /dev/vda"
        umount /mnt
        mount -t ext4 /dev/vda /mnt
        exec switch_root /mnt /sbin/init
    fi
    umount /mnt 2>/dev/null
fi

echo
echo "rvemu RV64 — Alpine $(cat /etc/alpine-release 2>/dev/null) (riscv64) [initramfs]"
echo RVEMU_SHELL_READY
exec /bin/sh -i
