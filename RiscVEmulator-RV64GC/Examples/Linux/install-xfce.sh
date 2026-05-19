#!/bin/sh
# install-xfce.sh — runs INSIDE the Alpine minirootfs guest (riscv64). Builds a
# full Alpine + IceWM desktop onto /dev/vda. apk runs natively, so every package
# trigger/scriptlet executes correctly. Driven by `Examples.Linux --rv64`.
#
# IceWM (not XFCE): XFCE's xfce4-session orchestration hangs on the emulated
# core, and its compositor/panel/daemons are too heavy for ~35 MIPS. IceWM is
# a single self-contained WM+taskbar — no D-Bus session, far faster.
set -x
MNT=/mnt
REPO_M=http://dl-cdn.alpinelinux.org/alpine/latest-stable/main
REPO_C=http://dl-cdn.alpinelinux.org/alpine/latest-stable/community

# ── networking — slirp NAT: gateway 10.0.2.2, DNS 10.0.2.3 ──
ip link set eth0 up
echo "nameserver 10.0.2.3" > /etc/resolv.conf
udhcpc -i eth0 -n -q -t 10 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf
# Pin the mirror so the install never depends on slirp DNS mid-stream.
echo "199.232.190.132 dl-cdn.alpinelinux.org" >> /etc/hosts

# ── live-system repos; mount the target disk (pre-formatted ext4 on host) ──
printf '%s\n%s\n' "$REPO_M" "$REPO_C" > /etc/apk/repositories
apk update
mkdir -p $MNT
mount /dev/vda $MNT
mkdir -p $MNT/etc/apk/keys
cp /etc/apk/keys/* $MNT/etc/apk/keys/ 2>/dev/null

# ── install the system into the disk (apk runs scriptlets natively) ──
APK="apk --root $MNT --repository $REPO_M --repository $REPO_C"
$APK --initdb add alpine-base
$APK add openrc eudev udev-init-scripts dbus dbus-x11
$APK add xorg-server xf86-video-fbdev xf86-input-libinput xf86-input-evdev xinit xrandr
$APK add font-dejavu ttf-dejavu
$APK add icewm xterm
$APK add alsa-lib alsa-utils
$APK add pcmanfm mpg123
$APK add netsurf || $APK add dillo || true
$APK add badwolf || true   # modern webkit2gtk browser
echo "RVEMU_INSTALL_PKGS_DONE"

# ── configure the new system ──
echo rvemu-rv64 > $MNT/etc/hostname
echo "nameserver 10.0.2.3" > $MNT/etc/resolv.conf
# The hostname MUST resolve, or xauth + dbus-launch fail and the XFCE
# session dies in a startx respawn loop.
printf '127.0.0.1\tlocalhost localhost.localdomain rvemu-rv64\n::1\tlocalhost localhost.localdomain rvemu-rv64\n' > $MNT/etc/hosts
printf '%s\n%s\n' "$REPO_M" "$REPO_C" > $MNT/etc/apk/repositories

cat > $MNT/etc/fstab <<EOF
/dev/vda  /        ext4   rw,relatime  0 1
proc      /proc    proc   defaults     0 0
sysfs     /sys     sysfs  defaults     0 0
devpts    /dev/pts devpts defaults     0 0
tmpfs     /tmp     tmpfs  defaults     0 0
EOF

# OpenRC runlevels — enabled natively via chroot.
chroot $MNT /bin/sh -c '
  rc-update add devfs sysinit
  rc-update add dmesg sysinit
  rc-update add udev sysinit
  rc-update add udev-trigger sysinit
  rc-update add hostname boot
  rc-update add bootmisc boot
  rc-update add sysctl boot
  rc-update add modules boot
  rc-update add syslog boot
  rc-update add dbus default
  rc-update add local default
  rc-update add networking boot
  passwd -d root
' 2>/dev/null || true

# Networking: DHCP eth0 on the slirp NAT. Both the standard `networking`
# service (interfaces file) and a local.d hook — whichever the image
# supports — bring eth0 up at boot.
cat > $MNT/etc/network/interfaces <<'EOF'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
EOF
mkdir -p $MNT/etc/local.d
cat > $MNT/etc/local.d/00-network.start <<'EOF'
#!/bin/sh
ip link set eth0 up
udhcpc -i eth0 -n -q -t 15 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf
EOF
chmod +x $MNT/etc/local.d/00-network.start

# startx launcher (runs on tty1 from inittab).
cat > $MNT/usr/bin/rvemu-startx <<'EOF'
#!/bin/sh
export HOME=/root
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR"
exec startx -- vt1 -keeptty > /var/log/startx.log 2>&1
EOF
chmod +x $MNT/usr/bin/rvemu-startx

# .xinitrc — start the MMIO→uinput input bridge, open a terminal, then the
# IceWM desktop. icewm-session brings up icewm + icewmbg + the taskbar.
cat > $MNT/root/.xinitrc <<'EOF'
/usr/bin/rvemu-rv64-input &
xterm -fa "DejaVu Sans Mono" -fs 12 -geometry 92x28+40+40 &
exec icewm-session
EOF
chmod +x $MNT/root/.xinitrc

# inittab — OpenRC boot, autologin serial shell, autostart X on tty1.
cat > $MNT/etc/inittab <<'EOF'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
ttyS0::respawn:/bin/login -f root
tty1::respawn:/usr/bin/rvemu-startx
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
EOF

# Force Xorg onto the simple-framebuffer /dev/fb0.
mkdir -p $MNT/etc/X11/xorg.conf.d
cat > $MNT/etc/X11/xorg.conf.d/10-fbdev.conf <<'EOF'
Section "Device"
  Identifier "fb0"
  Driver     "fbdev"
  Option     "fbdev" "/dev/fb0"
EndSection
Section "Screen"
  Identifier "screen0"
  Device     "fb0"
EndSection
EOF

# the MMIO→uinput input bridge (baked into the initramfs alongside this script).
cp /rvemu-rv64-input $MNT/usr/bin/rvemu-rv64-input
chmod +x $MNT/usr/bin/rvemu-rv64-input

# the ALSA→MMIO audio bridge daemon (also baked into the initramfs).
cp /rvemu-rv64-audio $MNT/usr/bin/rvemu-rv64-audio
chmod +x $MNT/usr/bin/rvemu-rv64-audio

# ALSA routing — the rv64 kernel has no sound card, so alsa-lib's userspace
# `file` plugin pipes the default PCM into a FIFO that rvemu-rv64-audio drains
# into the audio MMIO (host LinuxSdlAudio plays it).
cat > $MNT/etc/asound.conf <<'EOF'
pcm.rvemu_null { type null }
pcm.rvemu_file {
    type file
    slave.pcm "rvemu_null"
    file "/tmp/rvemu-audio.fifo"
    format raw
}
pcm.!default {
    type plug
    slave {
        pcm "rvemu_file"
        rate 44100
        channels 2
        format S16_LE
    }
}
EOF
cat > $MNT/etc/local.d/01-audio.start <<'EOF'
#!/bin/sh
[ -x /usr/bin/rvemu-rv64-audio ] && /usr/bin/rvemu-rv64-audio >/var/log/rvemu-audio.log 2>&1 &
EOF
chmod +x $MNT/etc/local.d/01-audio.start

# IceWM menu — give the desktop a launcher beyond the browser.
mkdir -p $MNT/root/.icewm
cat > $MNT/root/.icewm/menu <<'EOF'
prog "Terminal" - xterm
prog "Files" - pcmanfm
prog "Web (badwolf/webkit)" - badwolf
prog "Web (netsurf)" - netsurf
prog "Packages (apk)" - xterm -e sh -c "apk list -I 2>/dev/null | sort | less; exec sh"
prog "Music test (sine)" - xterm -e sh -c "speaker-test -t sine -f 440 -c 2 -l 1; exec sh"
EOF

sync
umount $MNT
echo "RVEMU_INSTALL_DONE"
poweroff -f
