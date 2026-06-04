/* vid_rvemu.c — Quake video driver against the RV32I emulator's
 * FramebufferDevice at 0x20000000 (320×200 RGBA8888) plus the
 * DisplayControlDevice at 0x20100000.
 *
 * Quake draws into vid_buffer (320×200 indexed-8). VID_Update palette-
 * converts the visible rectangles to the host FB. d_8to24table is
 * populated in VID_SetPalette from the 768-byte RGB palette Quake hands us.
 */

#include "common.h"
#include "d_local.h"
#include "quakedef.h"

#ifdef NQ_HACK
#include "host.h"
#endif

/* ── MMIO ──────────────────────────────────────────────────────── */
#define FB_BASE       ((volatile unsigned int *)0x20000000)   /* RGBA8888 */
#define DISP_WIDTH    (*(volatile unsigned int *)0x20100000)
#define DISP_HEIGHT   (*(volatile unsigned int *)0x20100004)
#define DISP_VSYNC    (*(volatile unsigned int *)0x2010000C)

viddef_t vid;

/* Render resolution — bumped from 320×200 to 640×400. ~4× the pixels;
 * surface cache scales linearly with pixel count, so 4 MB instead of
 * 1 MB. Keep the 4:3 aspect — Quake's HUD/menu artwork is laid out
 * assuming that ratio, and anything else stretches the status bar.
 * Crank to 800×600 if you still have FPS headroom (~7× pixels, needs
 * 6 MB surfcache). */
#define BASEWIDTH  640
#define BASEHEIGHT 400

byte  vid_buffer[BASEWIDTH * BASEHEIGHT];
short zbuffer  [BASEWIDTH * BASEHEIGHT];
byte  surfcache[4 * 1024 * 1024];      /* ~4 MB for 640×400 */

unsigned short d_8to16table[256];
unsigned       d_8to24table[256];

void VID_GetDesktopRect(vrect_t *r) { r->x = r->y = 0; r->width = r->height = 0; }
void VID_ShiftPalette(const byte *palette) { VID_SetPalette(palette); }

void
VID_SetPalette(const byte *palette)
{
    for (int i = 0; i < 256; i++) {
        unsigned r = palette[i*3 + 0];
        unsigned g = palette[i*3 + 1];
        unsigned b = palette[i*3 + 2];
        /* RGBA8888 little-endian → 0xAABBGGRR */
        d_8to24table[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    d_8to24table[255] &= 0x00FFFFFFu;   /* transparent index */
}

void
VID_Init(const byte *palette)
{
    vid.width      = vid.conwidth  = BASEWIDTH;
    vid.height     = vid.conheight = BASEHEIGHT;
    vid.aspect     = 1.0f;
    vid.numpages   = 1;
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer     = vid.conbuffer    = vid_buffer;
    vid.rowbytes   = vid.conrowbytes  = BASEWIDTH;

    d_pzbuffer = zbuffer;
    D_InitCaches(surfcache, sizeof(surfcache));

    VID_SetPalette(palette);

    /* Tell the host display: 320×200, vsync ready. */
    DISP_WIDTH  = BASEWIDTH;
    DISP_HEIGHT = BASEHEIGHT;
}

void VID_InitColormap(const byte *palette) { (void)palette; }
void VID_Shutdown(void) { }

void
VID_Update(vrect_t *rects)
{
    /* Palette-convert vid_buffer → FB. We always blit the whole frame; the
     * tile-based dirty-rect approach is more code than it saves at this
     * scale (one frame = 64k pixels). */
    (void)rects;
    volatile unsigned int *fb = FB_BASE;
    const byte *src = vid_buffer;
    for (int i = 0; i < BASEWIDTH * BASEHEIGHT; i++)
        fb[i] = d_8to24table[src[i]];
    DISP_VSYNC = 1;
}

void D_BeginDirectRect(int x, int y, const byte *p, int w, int h) { (void)x;(void)y;(void)p;(void)w;(void)h; }
void D_EndDirectRect  (int x, int y, int w, int h)                { (void)x;(void)y;(void)w;(void)h; }

qboolean VID_CheckAdequateMem(int w, int h) { (void)w;(void)h; return true; }
void     VID_ProcessEvents()                { }
void     VID_LockBuffer(void)               { }
void     VID_UnlockBuffer(void)             { }
void     VID_AddCommands()                  { }
void     VID_RegisterVariables()            { }
qboolean VID_SetMode(const qvidmode_t *m, const byte *p) { (void)m;(void)p; return false; }
void     VID_SetDefaultMode()               { }
qboolean window_visible(void)               { return true; }
