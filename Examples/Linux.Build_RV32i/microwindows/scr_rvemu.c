/*
 * scr_rvemu.c — Microwindows screen driver for the RV32I emulator's
 * direct-mapped framebuffer.
 *
 * The emulator exposes a 1024x768 32-bpp ABGR framebuffer at guest-physical
 * 0x85C00000 (last 4 MB of RAM, see CLAUDE.md "Don't put simple-framebuffer
 * in the DT"). The guest kernel intentionally has /memory shrunk to exclude
 * this region so userspace must mmap /dev/mem to reach it.
 *
 * Built when SCREEN=RVEMU in src/config.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "device.h"
#include "fb.h"
#include "genmem.h"
#include "genfont.h"

#define RVEMU_FB_BASE   0x85C00000UL
#define RVEMU_FB_W      1024
#define RVEMU_FB_H      768
#define RVEMU_FB_BPP    32
#define RVEMU_FB_PITCH  (RVEMU_FB_W * 4)
#define RVEMU_FB_SIZE   (RVEMU_FB_PITCH * RVEMU_FB_H)

static int   mem_fd = -1;
static PSD   rvemu_open(PSD psd);
static void  rvemu_close(PSD psd);
static void  rvemu_setpalette(PSD psd, int first, int count, MWPALENTRY *pal);

SCREENDEVICE scrdev = {
    0, 0, 0, 0, 0, 0, 0, NULL, 0, NULL, 0, 0, 0, 0, 0, 0,
    gen_fonts,
    rvemu_open,
    rvemu_close,
    rvemu_setpalette,
    gen_getscreeninfo,
    gen_allocatememgc,
    gen_mapmemgc,
    gen_freememgc,
    gen_setportrait,
    NULL,        /* Update */
    NULL         /* PreSelect */
};

static PSD
rvemu_open(PSD psd)
{
    PSUBDRIVER subdriver;

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        EPRINTF("scr_rvemu: open /dev/mem failed: %m\n");
        return NULL;
    }

    psd->xres = psd->xvirtres = RVEMU_FB_W;
    psd->yres = psd->yvirtres = RVEMU_FB_H;
    psd->planes = 1;
    psd->bpp = RVEMU_FB_BPP;
    psd->ncolors = (1 << 24);
    psd->pitch = RVEMU_FB_PITCH;
    psd->size = RVEMU_FB_SIZE;
    psd->flags = PSF_SCREEN;
    psd->pixtype = MWPF_TRUECOLORABGR;
    psd->portrait = MWPORTRAIT_NONE;
    psd->data_format = set_data_format(psd);

    psd->addr = mmap(NULL, RVEMU_FB_SIZE,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     mem_fd, RVEMU_FB_BASE);
    if (psd->addr == NULL || psd->addr == (unsigned char *)-1) {
        EPRINTF("scr_rvemu: mmap /dev/mem @ %#lx failed: %m\n", RVEMU_FB_BASE);
        close(mem_fd);
        mem_fd = -1;
        return NULL;
    }

    subdriver = select_fb_subdriver(psd);
    if (!subdriver) {
        EPRINTF("scr_rvemu: no subdriver for %dbpp pixtype %d\n",
                psd->bpp, psd->pixtype);
        munmap(psd->addr, RVEMU_FB_SIZE);
        close(mem_fd);
        mem_fd = -1;
        return NULL;
    }
    psd->orgsubdriver = subdriver;
    set_subdriver(psd, subdriver);

    EPRINTF("scr_rvemu: %dx%dx%dbpp ABGR @ %#lx\n",
            psd->xres, psd->yres, psd->bpp, RVEMU_FB_BASE);
    return psd;
}

static void
rvemu_close(PSD psd)
{
    if (psd->addr && psd->addr != (unsigned char *)-1) {
        munmap(psd->addr, RVEMU_FB_SIZE);
        psd->addr = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

static void
rvemu_setpalette(PSD psd, int first, int count, MWPALENTRY *pal)
{
    /* truecolor — no palette */
    (void)psd; (void)first; (void)count; (void)pal;
}

/*
 * kbd_ttyscan.c saves/restores the linux text-mode palette around graphics
 * activation. We have no text mode, so these are no-ops — but we still
 * need to define them since kbd_ttyscan.c references them unconditionally.
 */
void
ioctl_getpalette(int start, int len, short *red, short *green, short *blue)
{
    (void)start; (void)len; (void)red; (void)green; (void)blue;
}

void
ioctl_setpalette(int start, int len, short *red, short *green, short *blue)
{
    (void)start; (void)len; (void)red; (void)green; (void)blue;
}
