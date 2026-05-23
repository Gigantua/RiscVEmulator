#!/bin/sh
# /usr/bin/quake — wrapper that stops nano-X (so it doesn't paint over
# Quake's framebuffer output), runs the real binary in /usr/libexec, and
# restarts the WM on exit. Same pattern as /usr/bin/doom.

# Stop the WM if it's running.
[ -f /var/run/nanowm.pid ] && kill "$(cat /var/run/nanowm.pid)" 2>/dev/null
[ -f /var/run/nano-X.pid ] && kill "$(cat /var/run/nano-X.pid)" 2>/dev/null
sleep 1

cd /usr/share/games/quake
/usr/libexec/quake "$@"
ret=$?

# Restart the WM if S45microwindows is the init we came from.
[ -x /etc/init.d/S45microwindows ] && /etc/init.d/S45microwindows start

exit $ret
