################################################################################
#
# tyrquake — Quake software renderer on the rvemu nommu RV32 Linux guest.
#
# Source layout: the package directory ships
#   - a tiny tyrquake-rvemu glue (vid_rvemu.c + in_rvemu.c + Makefile)
#     under src/, ~500 LOC total
#   - the upstream TyrQuake tarball is fetched from GitHub
#
# This mirrors the doom-puredoom pattern: the bulk of the source comes
# from upstream, the rvemu-specific drivers live next to the package
# and get layered into the build directory before `make` runs.
#
################################################################################

TYRQUAKE_VERSION = master
TYRQUAKE_SITE    = git://disenchant.net/tyrquake
TYRQUAKE_SITE_METHOD = git
TYRQUAKE_LICENSE = GPL-2.0
TYRQUAKE_LICENSE_FILES = gnu.txt

# The rvemu-specific drivers + Makefile live in this package directory.
TYRQUAKE_PKGSRC = $(TYRQUAKE_PKGDIR)/src

# pak0.pak — freely-redistributable shareware data hosted on libsdl.org.
TYRQUAKE_PAK0_URL = https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz

define TYRQUAKE_PRE_BUILD_HOOKS
	# Stage our rvemu vid driver over the upstream vid_null.c (the
	# Makefile.rvemu we install excludes vid_null from the link).
	cp $(TYRQUAKE_PKGSRC)/vid_rvemu.c    $(@D)/common/vid_rvemu.c
	cp $(TYRQUAKE_PKGSRC)/Makefile.rvemu $(@D)/Makefile.rvemu

	# Fetch + extract pak0.pak from the SDL mirror if we don't have it
	# already. Skipped offline by checking for the local cache file.
	if [ ! -f $(BUILD_DIR)/tyrquake-pak0.pak ]; then \
	    wget -q -O $(BUILD_DIR)/quakesw.tar.gz "$(TYRQUAKE_PAK0_URL)" && \
	    tar xzf $(BUILD_DIR)/quakesw.tar.gz -C $(BUILD_DIR) && \
	    mv $(BUILD_DIR)/id1/pak0.pak $(BUILD_DIR)/tyrquake-pak0.pak && \
	    rm -rf $(BUILD_DIR)/id1 $(BUILD_DIR)/quakesw.tar.gz; \
	fi
endef
TYRQUAKE_POST_EXTRACT_HOOKS += TYRQUAKE_PRE_BUILD_HOOKS

define TYRQUAKE_BUILD_CMDS
	# Use our flat Makefile (Makefile.rvemu) rather than upstream's
	# auto-detection one — buildroot can't usefully feed it a host
	# uname result.
	$(MAKE) -C $(@D) -f Makefile.rvemu clean || true
	$(MAKE) -C $(@D) -f Makefile.rvemu \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Os -fPIC -fsigned-char \
		       -fno-strict-aliasing -DTYR_VERSION=0.71 \
		       -DTYR_VERSION_NUM=0.71 -DTYR_VERSION_TIME=1700000000LL" \
		LDFLAGS="$(TARGET_LDFLAGS) -static -Wl,-elf2flt=-r"
endef

define TYRQUAKE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tyrquake $(TARGET_DIR)/usr/libexec/quake
	$(INSTALL) -D -m 0755 $(TYRQUAKE_PKGSRC)/quake.sh $(TARGET_DIR)/usr/bin/quake
	$(INSTALL) -D -m 0644 $(BUILD_DIR)/tyrquake-pak0.pak \
	    $(TARGET_DIR)/usr/share/games/quake/id1/pak0.pak
	# rvemu-taskbar / desktop launcher (mtime-polled at /etc/rvemu-launchers.d)
	$(INSTALL) -D -m 0644 $(TYRQUAKE_PKGSRC)/quake.desktop \
	    $(TARGET_DIR)/etc/rvemu-launchers.d/quake.desktop
endef

$(eval $(generic-package))
