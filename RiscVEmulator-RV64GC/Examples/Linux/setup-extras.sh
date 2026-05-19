#!/bin/sh
# setup-extras.sh — runs INSIDE the Alpine guest (via --auto-commands).
# Configures ALSA→FIFO sound routing, installs a file manager / browser /
# tools, writes an icewm menu so the desktop has more than a browser. poweroff.
set -x

# ── net up so apk works ──
ip link set eth0 up
udhcpc -i eth0 -n -q -t 15 2>/dev/null || ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf

# ── extra desktop apps: file manager, mp3 player, a webkit browser if built ──
apk add pcmanfm mpg123 || true
apk add surf || true

# ── ALSA: the rv64 kernel has no sound card, so route the default PCM through
#    alsa-lib's userspace `file` plugin into a FIFO that rvemu-rv64-audio drains ──
cat > /etc/asound.conf <<'EOF'
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

# ── start the audio bridge daemon at boot (the 'local' service is enabled) ──
mkdir -p /etc/local.d
cat > /etc/local.d/01-audio.start <<'EOF'
#!/bin/sh
[ -x /usr/bin/rvemu-rv64-audio ] && /usr/bin/rvemu-rv64-audio >/var/log/rvemu-audio.log 2>&1 &
EOF
chmod +x /etc/local.d/01-audio.start

# ── icewm menu — give the desktop a launcher beyond the browser ──
mkdir -p /root/.icewm
cat > /root/.icewm/menu <<'EOF'
prog "Terminal" - xterm
prog "Files" - pcmanfm
prog "Web Browser" - netsurf
prog "Web (surf/webkit)" - surf
prog "Packages (apk)" - xterm -e sh -c "apk list -I 2>/dev/null | sort | less; exec sh"
prog "Music test (sine)" - xterm -e sh -c "speaker-test -t sine -f 440 -c 2 -l 1; exec sh"
EOF

sync
echo RVEMU_EXTRAS_DONE
poweroff -f
