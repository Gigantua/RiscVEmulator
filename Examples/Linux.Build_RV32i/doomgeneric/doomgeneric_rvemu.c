/*
 * doomgeneric_rvemu.c — nano-X backend for ozkl/doomgeneric.
 *
 * This is a normal Microwindows / nano-X client: opens a window,
 * uploads each game frame via GrArea(), and reads keystrokes from
 * nano-X events. Drag, focus, close-box, alt-tab, and the rest of the
 * desktop keep painting around it.
 *
 * Doom's internal frame is 320x200 ARGB. We upscale 2x to 640x400 in
 * a heap buffer (with R/B swap, since our nano-X server is configured
 * MWPF_TRUECOLORABGR), then GrArea blits it into the window each frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <microwin/nano-X.h>
#include "doomkeys.h"
#include "doomgeneric.h"

#define DOOM_W   DOOMGENERIC_RESX     /* 320 */
#define DOOM_H   DOOMGENERIC_RESY     /* 200 */
#define WIN_W    DOOM_W               /* native — upscaling costs bandwidth */
#define WIN_H    DOOM_H               /* the WM lets you resize the window */

/* Cap on-screen frame rate. Doom logic still ticks at 35 Hz; we just
 * skip GrArea uploads that arrive sooner than this many ms after the
 * previous one. Without throttling, 35 Hz × 256 KB ≈ 9 MB/s of unix-
 * socket traffic which starves the nano-X server's event loop and
 * freezes mouse input. */
#define DRAW_INTERVAL_MS  66          /* ~15 fps */

static GR_WINDOW_ID win;
static GR_GC_ID     gc;
static uint32_t    *up_buf;            /* ABGR scratch, WIN_W * WIN_H */
static uint32_t     last_draw_ms;

/* Key queue — DG_GetKey pops; nano-X event handler pushes. */
#define KEYQ 64
static uint16_t kq[KEYQ];
static unsigned kq_r, kq_w;

static void
kq_push(int pressed, unsigned char key)
{
    unsigned next = (kq_w + 1) % KEYQ;
    if (next == kq_r) return;
    kq[kq_w] = (uint16_t)((pressed ? 1 : 0) << 8) | key;
    kq_w = next;
}

/* MWKEY (nano-X keysym, mostly ASCII for printable keys + MWKEY_* for
 * specials) → doom keycode (doomkeys.h). */
static unsigned char
mwkey_to_doom(unsigned key)
{
    switch (key) {
    case MWKEY_LEFT:     return KEY_LEFTARROW;
    case MWKEY_RIGHT:    return KEY_RIGHTARROW;
    case MWKEY_UP:       return KEY_UPARROW;
    case MWKEY_DOWN:     return KEY_DOWNARROW;
    case MWKEY_LCTRL: case MWKEY_RCTRL:   return KEY_FIRE;
    case ' ':                              return KEY_USE;
    case MWKEY_LSHIFT: case MWKEY_RSHIFT:  return KEY_RSHIFT;
    case MWKEY_LALT:   case MWKEY_RALT:    return KEY_LALT;
    /* MWKEY_ENTER/TAB/BACKSPACE/ESCAPE are the same ints as their ASCII
     * counterparts (13, 9, 8, 27) — listing both would duplicate the
     * switch case. The single byte handles both. */
    case '\r':                             return KEY_ENTER;
    case 0x1b:                             return KEY_ESCAPE;
    case '\t':                             return KEY_TAB;
    case '\b': case 0x7f:                  return KEY_BACKSPACE;
    case MWKEY_F1:  return KEY_F1;  case MWKEY_F2:  return KEY_F2;
    case MWKEY_F3:  return KEY_F3;  case MWKEY_F4:  return KEY_F4;
    case MWKEY_F5:  return KEY_F5;  case MWKEY_F6:  return KEY_F6;
    case MWKEY_F7:  return KEY_F7;  case MWKEY_F8:  return KEY_F8;
    case MWKEY_F9:  return KEY_F9;  case MWKEY_F10: return KEY_F10;
    case MWKEY_F11: return KEY_F11; case MWKEY_F12: return KEY_F12;
    case '-':                              return KEY_MINUS;
    case '=':                              return KEY_EQUALS;
    case 'y': case 'Y': return 'y';  case 'n': case 'N': return 'n';
    }
    if (key >= 'a' && key <= 'z') return (unsigned char)key;
    if (key >= 'A' && key <= 'Z') return (unsigned char)(key | 0x20);
    if (key >= '0' && key <= '9') return (unsigned char)key;
    return 0;
}

static void
drain_events(void)
{
    GR_EVENT ev;
    while (GrCheckNextEvent(&ev), ev.type != GR_EVENT_TYPE_NONE) {
        if (ev.type == GR_EVENT_TYPE_CLOSE_REQ) {
            GrClose();
            exit(0);
        }
        if (ev.type == GR_EVENT_TYPE_KEY_DOWN || ev.type == GR_EVENT_TYPE_KEY_UP) {
            unsigned char dk = mwkey_to_doom(ev.keystroke.ch);
            if (dk) kq_push(ev.type == GR_EVENT_TYPE_KEY_DOWN, dk);
        }
        /* EXPOSURE: ignored — DG_DrawFrame blits the whole window every tick. */
    }
}

void DG_Init(void)
{
    if (GrOpen() < 0) {
        fprintf(stderr, "doomgeneric_rvemu: GrOpen failed (is nano-X running?)\n");
        exit(1);
    }
    win = GrNewWindowEx(GR_WM_PROPS_NOAUTORESIZE, "Doom",
                       GR_ROOT_WINDOW_ID, 0, 0, WIN_W, WIN_H, GR_RGB(0, 0, 0));
    GrSelectEvents(win,
                  GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP |
                  GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ);
    GrMapWindow(win);
    gc = GrNewGC();

    up_buf = (uint32_t *)malloc(WIN_W * WIN_H * sizeof(uint32_t));
    if (!up_buf) { fprintf(stderr, "doomgeneric_rvemu: malloc up_buf failed\n"); exit(1); }
}

void DG_DrawFrame(void)
{
    drain_events();

    /* Doom's DG_ScreenBuffer is 320x200 ARGB (uint32). Upscale 2x and
     * swap R↔B so it matches the screen's MWPF_TRUECOLORABGR layout. */
    const uint32_t *src = DG_ScreenBuffer;
    for (int y = 0; y < DOOM_H; y++) {
        uint32_t *row0 = up_buf + (y * SCALE)     * WIN_W;
        uint32_t *row1 = up_buf + (y * SCALE + 1) * WIN_W;
        for (int x = 0; x < DOOM_W; x++) {
            uint32_t argb = src[y * DOOM_W + x];
            uint32_t r = (argb >> 16) & 0xFF;
            uint32_t g = (argb >>  8) & 0xFF;
            uint32_t b =  argb        & 0xFF;
            uint32_t abgr = 0xFF000000u | (b << 16) | (g << 8) | r;
            row0[x * SCALE    ] = abgr;
            row0[x * SCALE + 1] = abgr;
            row1[x * SCALE    ] = abgr;
            row1[x * SCALE + 1] = abgr;
        }
    }
    GrArea(win, gc, 0, 0, WIN_W, WIN_H, up_buf, MWPF_TRUECOLORABGR);
    GrFlush();
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000U + t.tv_nsec / 1000000U);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    if (kq_r == kq_w) return 0;
    uint16_t k = kq[kq_r];
    kq_r = (kq_r + 1) % KEYQ;
    *pressed = (k >> 8) & 0xFF;
    *doomKey = k & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    if (title) GrSetWindowTitle(win, title);
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
