# tyrquake — Quake (TyrQuake) for the rvemu nommu RV32 Linux guest

Buildroot package that compiles TyrQuake against uClibc-ng with a custom
framebuffer driver that maps the host emulator's 1024×768 RGBA buffer at
guest-physical `0x85C00000` (same surface `doom-puredoom` and `rvemu-fbtest`
use).

The shareware `pak0.pak` is freely redistributable; the package installs
it at `/usr/share/games/quake/id1/pak0.pak`. The user-facing wrapper at
`/usr/bin/quake` stops the nano-X window manager (so it doesn't paint over
Quake), runs the binary, and restarts the WM on exit — the same pattern as
`doom-puredoom`'s `/usr/bin/doom` wrapper.

## Source layout

```
src/
  vid_rvemu.c     — replaces tyrquake/common/vid_null.c. Mmaps /dev/mem
                     at FB_BASE, 2×-upscales 320×200 indexed → 640×400
                     RGBA centered on the 1024×768 surface.
  in_rvemu.c      — reads /dev/input/event0 (kbd) + event1 (mouse),
                     synthesises Key_Event() / IN_MouseMove().
  snd_oss.c       — reuses TyrQuake's OSS driver but pointed at our
                     audio peripheral via /dev/dsp -> rvemu-audiod fifo.
  quake.sh        — wrapper installed as /usr/bin/quake.
  Makefile        — replaces TyrQuake's Makefile with a flat list of TUs,
                     -march=rv32i -mabi=ilp32 -O2 -fno-strict-aliasing.
                     Honors CC/CFLAGS/LDFLAGS from buildroot.
```

## Approach (why a fork instead of an upstream port)

Upstream TyrQuake ships `vid_x11`, `vid_glx`, `vid_sgl`, `vid_sdl`, `vid_wgl`
— none of which we have on the nommu image (no X, no SDL, no GL). The
cleanest path is the same as `doom-puredoom`'s: write a tiny `vid_*` driver
that talks directly to `/dev/mem`, swap out the null sound driver for
something that hits `rvemu-audiod`, and let everything else compile vanilla.

The bare-metal version of this code (Examples/Quake) shares the same
palette-convert + blit logic — `vid_rvemu.c` here and there have
near-identical bodies, differing only in how the FB pointer is acquired
(`/dev/mem` mmap on Linux vs literal `0x20000000` on bare metal).

## Status

**Scaffolding only.** Buildroot package not yet wired into
`Examples.Linux.Build_RV32i/Program.cs`; the source files in `src/` are
specifications for the bare-metal Quake port to be ported across (the
glue layer is shared modulo `mmap` vs absolute address). Activating the
package requires:

1. Wiring `BR2_PACKAGE_TYRQUAKE=y` into the buildroot defconfig the
   Linux.Build_RV32i job uses.
2. Adding the `tyrquake/` directory to `BR2_GLOBAL_PATCH_DIR` /
   `BR2_PACKAGE_OVERRIDE_FILE` so the package is found.
3. Authoring a `tyrquake-${version}.tar.gz` source-cache entry pointing
   at the `sezero/tyrquake` GitHub tag.
4. Installing a `S46quake` overlay that registers `quake` in the
   Microwindows desktop menu.

The build-side glue is straightforward; the porting cost is concentrated
in `src/vid_rvemu.c` and `src/in_rvemu.c`, both of which can be lifted
verbatim from `Examples/Quake/Programs/` once the bare-metal port is
proven and the API surface settles.
