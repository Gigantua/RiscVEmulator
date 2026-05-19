#!/usr/bin/env python3
"""Fix upstream Microwindows typo: keymap_standard.h has KEY_P (index 25)
   mapped to 'o' instead of 'p', so the guest types 'o' for both keys."""
import os

p = os.path.expanduser("~/rvemu-mw/microwindows/src/drivers/keymap_standard.h")
t = open(p).read()
bad  = "'o', '[', ']', MWKEY_ENTER, MWKEY_LCTRL,"
good = "'p', '[', ']', MWKEY_ENTER, MWKEY_LCTRL,"
if good in t:
    print("ALREADY")
elif bad in t:
    open(p, "w").write(t.replace(bad, good, 1))
    print("PATCHED")
else:
    print("NEEDLE NOT FOUND")
