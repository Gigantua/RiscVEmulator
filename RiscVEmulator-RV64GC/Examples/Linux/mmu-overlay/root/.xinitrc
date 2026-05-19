#!/bin/sh
# X session: a matchbox desktop. matchbox-window-manager draws frames;
# matchbox-desktop is the full-screen icon launcher that reads the
# .desktop entries in /usr/share/applications (NetSurf, xterm, ...).
xsetroot -solid '#3060a0' 2>/dev/null
matchbox-window-manager -use_titlebar yes &
exec matchbox-desktop
