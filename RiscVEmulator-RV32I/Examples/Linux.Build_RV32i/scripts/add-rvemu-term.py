#!/usr/bin/env python3
"""Add rvemu-term TARGETS line to demos/nanox/Makefile (idempotent)."""
import os

p = os.path.expanduser("~/rvemu-mw/microwindows/src/demos/nanox/Makefile")
with open(p) as f:
    t = f.read()

if "rvemu-term" in t:
    print("ALREADY")
else:
    needle = "TARGETS += $(MW_DIR_BIN)/rvemu-taskbar\n"
    add    = needle + "TARGETS += $(MW_DIR_BIN)/rvemu-term\n"
    if needle in t:
        with open(p, "w") as f:
            f.write(t.replace(needle, add, 1))
        print("PATCHED")
    else:
        print("NEEDLE NOT FOUND")
