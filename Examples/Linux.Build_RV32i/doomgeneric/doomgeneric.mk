################################################################################
#
# doomgeneric — rvemu nommu-friendly Doom port
#
################################################################################

# Pinned to a specific commit so buildroot's strict-hash check is happy
# and HEAD movement doesn't break reproducibility. Bump via:
#   git ls-remote https://github.com/ozkl/doomgeneric.git refs/heads/master
DOOMGENERIC_VERSION = dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
DOOMGENERIC_SITE = https://github.com/ozkl/doomgeneric.git
DOOMGENERIC_SITE_METHOD = git
BR_NO_CHECK_HASH_FOR += $(DOOMGENERIC_SOURCE)
DOOMGENERIC_LICENSE = GPL-2.0
DOOMGENERIC_LICENSE_FILES = LICENSE.txt
DOOMGENERIC_DEPENDENCIES = doom-wad

# $(DOOMGENERIC_PKGDIR) is auto-set by buildroot to the directory of this
# .mk file. We re-copy the rvemu backend + Makefile as part of BUILD
# (not POST_EXTRACT) so iterating on the C source only requires a
# `make doomgeneric-rebuild` — buildroot's pkg-rebuild target wipes
# stamp_built but NOT stamp_extracted, so POST_EXTRACT hooks don't run.

define DOOMGENERIC_BUILD_CMDS
	cp $(DOOMGENERIC_PKGDIR)/doomgeneric_rvemu.c $(@D)/doomgeneric/doomgeneric_rvemu.c
	cp $(DOOMGENERIC_PKGDIR)/Makefile.rvemu      $(@D)/doomgeneric/Makefile.rvemu
	$(MAKE) -C $(@D)/doomgeneric -f Makefile.rvemu clean
	$(MAKE) -C $(@D)/doomgeneric -f Makefile.rvemu \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -DNORMALUNIX -DLINUX -Os -fPIC" \
		LDFLAGS="$(TARGET_LDFLAGS) -Wl,-elf2flt=-r -static"
endef

define DOOMGENERIC_INSTALL_TARGET_CMDS
	# Real binary goes to libexec so running `doomgeneric` from the
	# shell can't bypass the wrapper. The wrapper is what /usr/bin
	# exposes — it sets the WAD path and the conservative zone size
	# (3 MB; nommu fragments badly and the default 5 MB OOMs once the
	# desktop is up). stderr → /tmp/doom.log so a silent crash from a
	# taskbar click leaves something to diagnose.
	$(INSTALL) -D -m 0755 $(@D)/doomgeneric/doomgeneric \
		$(TARGET_DIR)/usr/libexec/doomgeneric
	$(INSTALL) -d $(TARGET_DIR)/usr/bin
	printf '#!/bin/sh\nexec /usr/libexec/doomgeneric -iwad /usr/share/games/doom/doom1.wad -mb 3 "$$@" >/tmp/doom.log 2>&1\n' \
		> $(TARGET_DIR)/usr/bin/doomgeneric
	chmod 0755 $(TARGET_DIR)/usr/bin/doomgeneric
	# Convenience alias /usr/bin/doom — same wrapper, shorter name.
	ln -sf doomgeneric $(TARGET_DIR)/usr/bin/doom
	# Taskbar entry — rvemu-taskbar polls this dir's mtime and adds the
	# button live once rvpkg drops the file.
	$(INSTALL) -d $(TARGET_DIR)/etc/rvemu-launchers.d
	printf 'Name=Doom\nExec=/usr/bin/doom\n' \
		> $(TARGET_DIR)/etc/rvemu-launchers.d/doom.desktop
endef

$(eval $(generic-package))
