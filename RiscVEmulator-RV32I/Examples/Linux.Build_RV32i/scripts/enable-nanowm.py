#!/usr/bin/env python3
import os
p = os.path.expanduser("~/rvemu-mw/microwindows/src/demos/nanox/Makefile")
t = open(p).read()
old = "#TARGETS += $(MW_DIR_BIN)/nanowm"
new = "TARGETS += $(MW_DIR_BIN)/nanowm"
if old in t:
    t = t.replace(old, new)
    open(p, "w").write(t)
    print("PATCHED")
elif new in t:
    print("ALREADY")
else:
    print("NOT FOUND")
