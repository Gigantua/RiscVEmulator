#!/bin/sh
# setup-browser.sh — runs in the Alpine guest. Probes for a modern (webkit)
# browser in the riscv64 repos and installs the lightest one available.
set -x
ip link set eth0 up
udhcpc -i eth0 -n -q -t 15 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf

echo "=== webkit2gtk in repo? ==="
apk search -q webkit2gtk 2>/dev/null
echo "=== candidate browsers in repo? ==="
for p in badwolf luakit surf epiphany midori netsurf-gtk3 dillo; do
    apk search -q -e "$p" 2>/dev/null
done

# install the lightest modern engine that resolves
for p in badwolf luakit epiphany midori; do
    if apk add "$p" 2>&1 | tail -2; then
        command -v "$p" >/dev/null 2>&1 && { echo "BROWSER_INSTALLED=$p"; break; }
    fi
done

echo "=== browsers now present ==="
for b in badwolf luakit surf epiphany midori netsurf dillo; do
    command -v "$b" >/dev/null 2>&1 && echo "  $b"
done
sync
echo RVEMU_BROWSER_DONE
poweroff -f
