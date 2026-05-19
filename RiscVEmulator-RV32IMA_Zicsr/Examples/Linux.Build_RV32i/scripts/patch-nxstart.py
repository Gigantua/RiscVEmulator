#!/usr/bin/env python3
"""Patch Microwindows nxstart for rvemu:
   - hardcoded PATH /bin/ → /usr/bin/ (where our overlay installs the apps)
   - prune Apps[] entries that reference binaries we don't ship
   - enable ROOT_WIN_RECOLOR so the desktop has a gray background instead
     of leaving black/uninit framebuffer behind every window
"""
import os, re

p = os.path.expanduser("~/rvemu-mw/microwindows/src/demos/nanox/nxstart.c")
t = open(p).read()
orig = t

# 1. Gray desktop background.
t = t.replace(
    "#define ROOT_WIN_RECOLOR    0",
    "#define ROOT_WIN_RECOLOR    1",
    1,
)

# 2. PATH /bin/ → /usr/bin/. The #if ELKS branch sets /bin/; the else
#    sets bin/. Replace both unconditionally so the resulting binary
#    just uses /usr/bin/ regardless of build flags.
t = t.replace(
    '#define PATH    "/bin/"',
    '#define PATH    "/usr/bin/"',
    1,
)
t = t.replace(
    '#define PATH    "bin/"',
    '#define PATH    "/usr/bin/"',
    1,
)

# 3. Replace Apps[] with only the binaries we actually ship. Keep
#    {"quit","quit"} and the trailing sentinel.
new_apps = '''} Apps[] = {
\t{"clock",      PATH "nxclock"},
\t{"eyes",       PATH "nxeyes"},
\t{"chess",      PATH "nxchess"},
\t{"calculator", PATH "nxcalc"},
\t{"hello",      PATH "demo-hello"},
\t{"arc",        PATH "demo-arc"},
\t{"blit",       PATH "demo-blit"},
\t{"quit",       "quit"},
\t{"", ""}
};
'''
t = re.sub(
    r"\}\s*Apps\[\]\s*=\s*\{[\s\S]*?\{\"\",\s*\"\"\}\s*\};",
    new_apps,
    t,
    count=1,
)

if t == orig:
    print("NO CHANGE (already patched or markers missing)")
else:
    open(p, "w").write(t)
    print("PATCHED")
