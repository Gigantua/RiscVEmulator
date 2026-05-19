#!/usr/bin/env python3
"""Add rvemu-taskbar TARGETS line to demos/nanox/Makefile."""
import os

p = os.path.expanduser("~/rvemu-mw/microwindows/src/demos/nanox/Makefile")
with open(p) as f:
    t = f.read()

if "rvemu-taskbar" in t:
    print("ALREADY")
else:
    needle = "\t$(MW_DIR_BIN)/demo-hello\n"
    add = needle + "\nTARGETS += $(MW_DIR_BIN)/rvemu-taskbar\n"
    if needle in t:
        t = t.replace(needle, add, 1)
        with open(p, "w") as f:
            f.write(t)
        print("PATCHED")
    else:
        print("NEEDLE NOT FOUND")
