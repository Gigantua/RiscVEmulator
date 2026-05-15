#!/usr/bin/env python3
"""Patch demos/tuxchess/images.c so IMAGE_PATH = /usr/share/nxchess
   (absolute path, since the chess binary runs from /usr/bin)."""
import os

p = os.path.expanduser("~/rvemu-mw/microwindows/src/demos/tuxchess/images.c")
t = open(p).read()
old = '#define IMAGE_PATH "demos/tuxchess/images"'
new = '#define IMAGE_PATH "/usr/share/nxchess"'
if new in t:
    print("ALREADY")
elif old in t:
    open(p, "w").write(t.replace(old, new, 1))
    print("PATCHED")
else:
    print("NEEDLE NOT FOUND")
