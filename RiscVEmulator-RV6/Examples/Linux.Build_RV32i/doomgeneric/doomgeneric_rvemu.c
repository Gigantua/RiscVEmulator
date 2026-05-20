/*
 * doomgeneric_rvemu.c — direct /dev/mem framebuffer backend for
 * ozkl/doomgeneric on the rvemu nommu guest.
 *
 * We tried windowed (nano-X client) Doom — it works architecturally but
 * the 256 KB-per-frame socket transfer to the nano-X server starves the
 * 60-MIPS emulated CPU. BUFFER_MMAP would fix it but nommu uClibc can't
 * MAP_SHARED a regular file on tmpfs, and shmget needs CONFIG_SYSVIPC
 * plus a working shm fs we don't have. So we go fullscreen: Doom takes
 * over the FB at /dev/mem 0x85C00000, paints directly, reads keys from
 * /dev/input/event0. When Doom exits the desktop comes back automatically
 * (nano-X repaints on its next tick).
 *
 * Provides the 5 doomgeneric callbacks: DG_Init/DrawFrame/SleepMs/GetTicksMs/GetKey.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

/* See doomgeneric_rvemu.c's previous version for why we don't pull in
 * <linux/input.h> — it #defines KEY_TAB / KEY_ENTER etc. that collide
 * with doomkeys.h. We copy just what we need. */
struct input_event {
    struct { long tv_sec, tv_usec; } time;
    unsigned short type, code;
    int            value;
};
#define EV_KEY 0x01
enum {
    LX_ESC = 1,   LX_1 = 2,  LX_2 = 3,  LX_3 = 4,  LX_4 = 5,  LX_5 = 6,
    LX_6 = 7,     LX_7 = 8,  LX_8 = 9,  LX_9 = 10, LX_0 = 11,
    LX_MINUS = 12, LX_EQUAL = 13, LX_BACKSPACE = 14, LX_TAB = 15,
    LX_Q = 16, LX_W = 17, LX_E = 18, LX_R = 19, LX_T = 20, LX_Y = 21,
    LX_U = 22, LX_I = 23, LX_O = 24, LX_P = 25,
    LX_ENTER = 28, LX_LEFTCTRL = 29,
    LX_A = 30, LX_S = 31, LX_D = 32, LX_F = 33, LX_G = 34, LX_H = 35,
    LX_J = 36, LX_K = 37, LX_L = 38,
    LX_LEFTSHIFT = 42, LX_Z = 44, LX_X = 45, LX_C = 46, LX_V = 47,
    LX_B = 48, LX_N = 49, LX_M = 50, LX_RIGHTSHIFT = 54, LX_LEFTALT = 56,
    LX_SPACE = 57,
    LX_F1 = 59, LX_F2, LX_F3, LX_F4, LX_F5, LX_F6, LX_F7, LX_F8, LX_F9, LX_F10,
    LX_UP = 103, LX_LEFT = 105, LX_RIGHT = 106, LX_DOWN = 108,
    LX_RIGHTALT = 100, LX_F11 = 87, LX_F12 = 88,
};

#include "doomkeys.h"
#include "doomgeneric.h"

#define FB_BASE   0x85C00000UL
#define FB_W      1024
#define FB_H      768
#define FB_BYTES  (FB_W * FB_H * 4)
#define DOOM_W    DOOMGENERIC_RESX     /* 640 in this build of doomgeneric */
#define DOOM_H    DOOMGENERIC_RESY     /* 400 — already 2x-upscaled internally */
/* 640x400 fits inside 1024x768 at native 1x with comfortable letterbox.
 * Trying to upscale further (3x → 1920x1200) would write past the FB and
 * corrupt whatever lives in the pages after it. */
#define DST_W     DOOM_W
#define DST_H     DOOM_H
#define OFF_X     ((FB_W - DST_W) / 2)        /* 192 */
#define OFF_Y     ((FB_H - DST_H) / 2)        /* 184 */

static volatile uint32_t *fb;
static int                kbd_fd = -1;

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

static unsigned char
linux_to_doom(uint16_t code)
{
    switch (code) {
    case LX_RIGHT:     return KEY_RIGHTARROW;
    case LX_LEFT:      return KEY_LEFTARROW;
    case LX_UP:        return KEY_UPARROW;
    case LX_DOWN:      return KEY_DOWNARROW;
    case LX_LEFTCTRL:  return KEY_FIRE;
    case LX_SPACE:     return KEY_USE;
    case LX_LEFTSHIFT: case LX_RIGHTSHIFT: return KEY_RSHIFT;
    case LX_LEFTALT:   case LX_RIGHTALT:   return KEY_LALT;
    case LX_ENTER:     return KEY_ENTER;
    case LX_ESC:       return KEY_ESCAPE;
    case LX_TAB:       return KEY_TAB;
    case LX_BACKSPACE: return KEY_BACKSPACE;
    case LX_F1: return KEY_F1; case LX_F2: return KEY_F2;
    case LX_F3: return KEY_F3; case LX_F4: return KEY_F4;
    case LX_F5: return KEY_F5; case LX_F6: return KEY_F6;
    case LX_F7: return KEY_F7; case LX_F8: return KEY_F8;
    case LX_F9: return KEY_F9; case LX_F10: return KEY_F10;
    case LX_F11: return KEY_F11; case LX_F12: return KEY_F12;
    case LX_Y: return 'y'; case LX_N: return 'n';
    case LX_A: return 'a'; case LX_D: return 'd';
    case LX_W: return 'w'; case LX_S: return 's';
    case LX_1: return '1'; case LX_2: return '2'; case LX_3: return '3';
    case LX_4: return '4'; case LX_5: return '5'; case LX_6: return '6';
    case LX_7: return '7'; case LX_8: return '8'; case LX_9: return '9';
    case LX_0: return '0';
    case LX_MINUS:     return KEY_MINUS;
    case LX_EQUAL:     return KEY_EQUALS;
    default: return 0;
    }
}

void DG_Init(void)
{
    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("open /dev/mem"); exit(1); }
    void *p = mmap(NULL, FB_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, mem, FB_BASE);
    close(mem);
    if (p == MAP_FAILED) { perror("mmap fb"); exit(1); }
    fb = (volatile uint32_t *)p;
    memset((void *)fb, 0, FB_BYTES);

    kbd_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0)
        fprintf(stderr, "doom: /dev/input/event0 unavailable — no keys\n");
}

static void
drain_keys(void)
{
    if (kbd_fd < 0) return;
    struct input_event ev;
    while (read(kbd_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        unsigned char dk = linux_to_doom(ev.code);
        if (dk) kq_push(ev.value != 0, dk);
    }
}

void DG_DrawFrame(void)
{
    drain_keys();

    /* 640x400 ARGB → centered on 1024x768 FB, no upscale (doomgeneric
     * already doubled internally). R↔B swap for our ABGR8888 layout. */
    const uint32_t *src = DG_ScreenBuffer;
    for (int y = 0; y < DOOM_H; y++) {
        volatile uint32_t *dst = fb + (OFF_Y + y) * FB_W + OFF_X;
        for (int x = 0; x < DOOM_W; x++) {
            uint32_t argb = src[y * DOOM_W + x];
            uint32_t r = (argb >> 16) & 0xFF;
            uint32_t g = (argb >>  8) & 0xFF;
            uint32_t b =  argb        & 0xFF;
            *dst++ = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
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

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
