/* vid_rvemu.c — TyrQuake software-renderer video driver against the rvemu
 * Linux guest's framebuffer (1024×768 RGBA8888 at guest-physical
 * 0x85C00000, mmapped through /dev/mem).
 *
 * Renders Quake's native 320×200 indexed-8 buffer 2×-upscaled (→ 640×400)
 * centered on the 1024×768 surface. Surrounding pixels left as-is, which
 * lets the doom/desktop UI peek through if Quake doesn't fill them — same
 * trick as Examples/Linux.Build_RV32i/doom-puredoom uses.
 *
 * Drop-in for tyrquake/common/vid_null.c — provided to the upstream tree
 * via the buildroot package's TYRQUAKE_PRE_BUILD_HOOKS.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>

#include "common.h"
#include "d_local.h"
#include "quakedef.h"
#include "host.h"

#define FB_BASE   0x85C00000UL
#define FB_W      1024
#define FB_H      768
#define BASE_W    320
#define BASE_H    200
#define DST_W     (BASE_W * 2)
#define DST_H     (BASE_H * 2)
#define DST_X     ((FB_W - DST_W) / 2)
#define DST_Y     ((FB_H - DST_H) / 2)

viddef_t vid;

static volatile uint32_t *g_fb;
byte  vid_buffer[BASE_W * BASE_H];
short zbuffer[BASE_W * BASE_H];
byte  surfcache[1024 * 1024];

unsigned short d_8to16table[256];
unsigned       d_8to24table[256];

void VID_GetDesktopRect(vrect_t *r) { r->x=r->y=0; r->width=FB_W; r->height=FB_H; }
void VID_ShiftPalette(const byte *p) { VID_SetPalette(p); }

void VID_SetPalette(const byte *palette)
{
    for (int i = 0; i < 256; i++) {
        unsigned r = palette[i*3+0], g = palette[i*3+1], b = palette[i*3+2];
        /* /dev/mem framebuffer is ABGR in the rvemu peripherals. */
        d_8to24table[i] = (0xFFu<<24) | (b<<16) | (g<<8) | r;
    }
    d_8to24table[255] &= 0x00FFFFFFu;
}

void VID_Init(const byte *palette)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) Sys_Error("/dev/mem open failed");
    void *p = mmap(0, FB_W*FB_H*4, PROT_READ|PROT_WRITE, MAP_SHARED, fd, FB_BASE);
    if (p == MAP_FAILED) Sys_Error("/dev/mem mmap @ %lx failed", FB_BASE);
    close(fd);
    g_fb = (volatile uint32_t *)p;

    vid.width      = vid.conwidth  = BASE_W;
    vid.height     = vid.conheight = BASE_H;
    vid.aspect     = 1.0f;
    vid.numpages   = 1;
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer     = vid.conbuffer    = vid_buffer;
    vid.rowbytes   = vid.conrowbytes  = BASE_W;

    d_pzbuffer = zbuffer;
    D_InitCaches(surfcache, sizeof(surfcache));

    VID_SetPalette(palette);
}

void VID_InitColormap(const byte *p) { (void)p; }
void VID_Shutdown(void) { munmap((void*)g_fb, FB_W*FB_H*4); g_fb = 0; }

void VID_Update(vrect_t *rects)
{
    (void)rects;
    /* 2× point upscale, palette-convert, blit. */
    for (int y = 0; y < BASE_H; y++) {
        const byte    *src = vid_buffer + y * BASE_W;
        volatile uint32_t *row0 = g_fb + (DST_Y + y*2)     * FB_W + DST_X;
        volatile uint32_t *row1 = g_fb + (DST_Y + y*2 + 1) * FB_W + DST_X;
        for (int x = 0; x < BASE_W; x++) {
            uint32_t c = d_8to24table[src[x]];
            row0[x*2  ] = c; row0[x*2+1] = c;
            row1[x*2  ] = c; row1[x*2+1] = c;
        }
    }
}

void D_BeginDirectRect(int x, int y, const byte *p, int w, int h) { (void)x;(void)y;(void)p;(void)w;(void)h; }
void D_EndDirectRect  (int x, int y, int w, int h)                { (void)x;(void)y;(void)w;(void)h; }

qboolean VID_CheckAdequateMem(int w, int h) { (void)w;(void)h; return true; }
void     VID_LockBuffer  (void) { }
void     VID_UnlockBuffer(void) { }
void     VID_AddCommands (void) { }
void     VID_RegisterVariables(void) { }
qboolean VID_SetMode(const qvidmode_t *m, const byte *p) { (void)m;(void)p; return false; }
void     VID_SetDefaultMode() { }
qboolean window_visible(void) { return true; }
void     VID_ProcessEvents()  { }
