#!/bin/sh
# setup-desktop.sh — runs INSIDE the booted Alpine guest (via --auto-commands).
# Brings networking up persistently and installs the audio userspace, then
# powers off. One-shot post-install tweak; install-xfce.sh bakes the same in
# for fresh installs.
set -x

# ── bring eth0 up now (slirp NAT) so apk works this boot ──
ip link set eth0 up
udhcpc -i eth0 -n -q -t 15 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf

# ── audio userspace ──
apk add alsa-lib alsa-utils || true

# ── persist networking: a local.d hook (the 'local' service is enabled) ──
mkdir -p /etc/local.d
cat > /etc/local.d/00-network.start <<'NET'
#!/bin/sh
ip link set eth0 up
udhcpc -i eth0 -n -q -t 15 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf
NET
chmod +x /etc/local.d/00-network.start

cat > /etc/network/interfaces <<'IF'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
IF
rc-update add networking boot 2>/dev/null || true

sync
echo RVEMU_SETUP_DONE
poweroff -f
