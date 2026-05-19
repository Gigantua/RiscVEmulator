#!/usr/bin/env python3
"""Register SCREEN=RVEMU and KEYBOARD=RVEMU in Microwindows drivers/Objects.rules."""
import os

path = os.path.expanduser("~/rvemu-mw/microwindows/src/drivers/Objects.rules")
with open(path) as f:
    txt = f.read()

# --- screen driver -----------------------------------------------------
screen_needle = "ifeq ($(SCREEN), FB)"
screen_insert = (
    "ifeq ($(SCREEN), RVEMU)\n"
    "MW_CORE_OBJS += $(MW_DIR_OBJ)/drivers/scr_rvemu.o\n"
    "endif\n"
    "\n"
)
if "scr_rvemu.o" not in txt:
    txt = txt.replace(screen_needle, screen_insert + screen_needle, 1)
    print("INJECTED SCREEN=RVEMU")
else:
    print("ALREADY: SCREEN=RVEMU")

# --- keyboard driver ---------------------------------------------------
kbd_needle = "ifeq ($(KEYBOARD), SCANKBD)"
kbd_insert = (
    "ifeq ($(KEYBOARD), RVEMU)\n"
    "MW_CORE_OBJS += $(MW_DIR_OBJ)/drivers/kbd_rvemu.o\n"
    "endif\n"
    "\n"
)
if "kbd_rvemu.o" not in txt:
    txt = txt.replace(kbd_needle, kbd_insert + kbd_needle, 1)
    print("INJECTED KEYBOARD=RVEMU")
else:
    print("ALREADY: KEYBOARD=RVEMU")

with open(path, "w") as f:
    f.write(txt)
