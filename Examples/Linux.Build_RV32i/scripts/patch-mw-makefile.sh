#!/bin/bash
# Strip fork-requiring nano-X demos from the Microwindows demos Makefile.
# These demos use fork() which doesn't work on nommu.
set -e
F="$HOME/rvemu-mw/microwindows/src/demos/nanox/Makefile"

# Reset to pristine state first
( cd "$HOME/rvemu-mw/microwindows" && git checkout -- src/demos/nanox/Makefile )

python3 - <<'PY'
import re
path = __import__('os').path.expanduser('~/rvemu-mw/microwindows/src/demos/nanox/Makefile')
with open(path) as f:
    txt = f.read()

# Remove TARGETS continuation lines that name fork-using demos
for name in ['nxroach', 'nxev', 'nxlaunch']:
    txt = re.sub(r'^\s*\$\(MW_DIR_BIN\)/' + name + r' \\\n', '', txt, flags=re.M)

# Remove the nxterm TARGETS += line (indented inside nested ifneq)
txt = re.sub(r'^\s*TARGETS \+= \$\(MW_DIR_BIN\)/nxterm\s*\n', '', txt, flags=re.M)

# Remove the entire C++ demos block (CANNYOBJS / AGGOBJS)
txt = re.sub(
    r'^#\s*\n# C\+\+ demos\n#\n(?:.*\n)*?TARGETS \+= \$\(MW_DIR_BIN\)/demo-agg\n',
    '', txt, flags=re.M)

with open(path, 'w') as f:
    f.write(txt)
PY

echo "--- after patch ---"
grep -n 'nxlaunch\|nxterm\|nxroach\|nxev\|CANNY\|AGGOBJS\|cannyedgedetect' "$F" | head -20 || echo "(none — clean)"
