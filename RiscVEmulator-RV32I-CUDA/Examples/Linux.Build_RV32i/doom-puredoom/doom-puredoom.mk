################################################################################
#
# doom-puredoom — PureDOOM port for the rvemu nommu RV32 Linux guest.
#
# Source layout: the package directory ships a tiny doom_linux.c (~250 LOC)
# that wires PureDOOM's callbacks to /dev/mem MMIO. PureDOOM.h itself
# (48k LOC, single-header amalgamation) is staged into the same dir by
# Examples.Linux.Build_RV32i before this package is built — it lives at
# Examples/Doom/Programs/PureDOOM.h and is shared with the bare-metal
# Examples.Doom demo, no duplication.
#
################################################################################

DOOM_PUREDOOM_VERSION = 1.0
DOOM_PUREDOOM_SITE = $(DOOM_PUREDOOM_PKGDIR)/src
DOOM_PUREDOOM_SITE_METHOD = local
DOOM_PUREDOOM_LICENSE = MIT
DOOM_PUREDOOM_DEPENDENCIES = doom-wad

define DOOM_PUREDOOM_BUILD_CMDS
	$(MAKE) -C $(@D) clean || true
	# -fsigned-char is LOAD-BEARING and MUST be passed here, not just in
	# the package Makefile: buildroot sets CFLAGS= on the make command
	# line, which overrides any `CFLAGS +=` inside the Makefile. RISC-V
	# GCC defaults `char` to unsigned; PureDOOM's ticcmd struct stores
	# negative move values in bare `char` fields, so without this flag
	# pressing S/A flings the player forward/right at ~9x speed.
	$(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Os -fPIC -fsigned-char" \
		LDFLAGS="$(TARGET_LDFLAGS) -static -Wl,-elf2flt=-r"
endef

define DOOM_PUREDOOM_INSTALL_TARGET_CMDS
	# Real binary in libexec; user-facing wrapper in /usr/bin owns the
	# nano-X stop/start dance (otherwise nxclock/nxeyes paint over Doom
	# and exit leaves a corrupted FB).
	$(INSTALL) -D -m 0755 $(@D)/doom $(TARGET_DIR)/usr/libexec/doom
	$(INSTALL) -d $(TARGET_DIR)/usr/bin
	# Defensive cleanup: an older doomgeneric install may have left
	# /usr/bin/doom as a symlink → doomgeneric. Shell `>` would follow
	# the symlink and we'd end up with a self-referential pair.
	rm -f $(TARGET_DIR)/usr/bin/doom $(TARGET_DIR)/usr/bin/doomgeneric
	printf '%s\n' \
		'#!/bin/sh' \
		'echo "doom-wrapper: stopping desktop + input..." >> /tmp/doom.log' \
		'/etc/init.d/S45microwindows stop >>/tmp/doom.log 2>&1 || true' \
		'/etc/init.d/S42input stop      >>/tmp/doom.log 2>&1 || true' \
		'sleep 1' \
		'echo "doom-wrapper: launching engine..." >> /tmp/doom.log' \
		'/usr/libexec/doom "$$@" >>/tmp/doom.log 2>&1' \
		'echo "doom-wrapper: restarting input + desktop..." >> /tmp/doom.log' \
		'/etc/init.d/S42input start     >>/tmp/doom.log 2>&1 || true' \
		'/etc/init.d/S45microwindows start >>/tmp/doom.log 2>&1 || true' \
		> $(TARGET_DIR)/usr/bin/doom
	chmod 0755 $(TARGET_DIR)/usr/bin/doom
	# Taskbar entry — rvemu-taskbar polls this dir's mtime and adds the
	# Doom button live without restart.
	$(INSTALL) -d $(TARGET_DIR)/etc/rvemu-launchers.d
	printf 'Name=Doom\nExec=/usr/bin/doom\n' \
		> $(TARGET_DIR)/etc/rvemu-launchers.d/doom.desktop
endef

$(eval $(generic-package))
