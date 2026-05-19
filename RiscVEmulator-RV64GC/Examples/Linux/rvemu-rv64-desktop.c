/*
 * rvemu-rv64-desktop.c — a self-contained framebuffer desktop for the rvemu
 * RV64 Alpine build.
 *
 * No X, no toolkit, no networking. It mmaps three physical regions through
 * /dev/mem — the framebuffer and the keyboard/mouse MMIO peripherals — then
 * draws a desktop with clickable tiles and runs a couple of built-in apps
 * (a paint canvas and a clock). Cross-compiled static so the glibc binary
 * runs on the musl Alpine rootfs unchanged.
 *
 *   FB      : 0xBFC00000, 1024x768 xRGB8888  (host FramebufferDevice)
 *   keyboard: 0x10001000  (+0 has-data, +4 pop -> KEY_*|0x100 on press)
 *   mouse   : 0x10002000  (+0 has-data, +4 dx, +8 dy, +0xC buttons)
 */
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>

#define FB_W       1024
#define FB_H       768
#define FB_BASE    0xBFC00000UL
#define FB_BYTES   ((size_t)FB_W * FB_H * 4)
#define KBD_BASE   0x10001000UL
#define MOUSE_BASE 0x10002000UL

#define RGB(r,g,b) ((uint32_t)(r) | ((uint32_t)(g)<<8) | ((uint32_t)(b)<<16) | 0xFF000000u)

/* Linux KEY_* codes — see LinuxSdlViewer's key map. */
#define KEY_ESC 1
#define KEY_C   46

static volatile uint32_t *fb;
static volatile uint32_t *kbd;      /* word 0 = has-data, 1 = pop, 2 = mods            */
static volatile uint32_t *mouse;    /* word 0 = has-data, 1 = dx, 2 = dy, 3 = buttons  */

/* Off-screen buffer — every primitive draws here; present() blits the whole
 * finished frame to the framebuffer in one pass, so the host renderer never
 * samples a half-drawn frame (which showed up as flicker). */
static uint32_t backbuf[FB_W * FB_H];

/* ── drawing primitives ───────────────────────────────────────────── */

static void fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = backbuf + (size_t)(y + yy) * FB_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

/* Blit the completed off-screen frame to the framebuffer in one pass. */
static void present(void)
{
    memcpy((void *)fb, backbuf, FB_BYTES);
}

static void frame(int x, int y, int w, int h, int t, uint32_t c)
{
    fill(x, y, w, t, c);  fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);  fill(x + w - t, y, t, h, c);
}

/* vertical gradient background */
static void gradient(uint32_t top, uint32_t bot)
{
    int tr = top & 0xFF, tg = (top >> 8) & 0xFF, tb = (top >> 16) & 0xFF;
    int br = bot & 0xFF, bg = (bot >> 8) & 0xFF, bb = (bot >> 16) & 0xFF;
    for (int y = 0; y < FB_H; y++) {
        int r = tr + (br - tr) * y / FB_H;
        int g = tg + (bg - tg) * y / FB_H;
        int b = tb + (bb - tb) * y / FB_H;
        fill(0, y, FB_W, 1, RGB(r, g, b));
    }
}

/* ── 7-segment digit (no font needed) ─────────────────────────────── */

static const uint8_t SEG[10] = {  /* bits: a b c d e f g */
    0x7E, 0x30, 0x6D, 0x79, 0x33, 0x5B, 0x5F, 0x70, 0x7F, 0x7B
};

static void digit7(int x, int y, int s, int d, uint32_t on, uint32_t off)
{
    int w = s * 4, t = s;
    uint8_t m = (d >= 0 && d <= 9) ? SEG[d] : 0;
    fill(x + t,     y,            w - 2*t, t,        (m & 0x40) ? on : off);  /* a top    */
    fill(x + w - t, y + t,        t,       w - 2*t,  (m & 0x20) ? on : off);  /* b t-right*/
    fill(x + w - t, y + w,        t,       w - 2*t,  (m & 0x10) ? on : off);  /* c b-right*/
    fill(x + t,     y + 2*w - t,  w - 2*t, t,        (m & 0x08) ? on : off);  /* d bottom */
    fill(x,         y + w,        t,       w - 2*t,  (m & 0x04) ? on : off);  /* e b-left */
    fill(x,         y + t,        t,       w - 2*t,  (m & 0x02) ? on : off);  /* f t-left */
    fill(x + t,     y + w - t/2,  w - 2*t, t,        (m & 0x01) ? on : off);  /* g middle */
}

/* ── tile icons ───────────────────────────────────────────────────── */

static void icon_paint(int cx, int cy)
{
    fill(cx - 26, cy + 10, 52, 10, RGB(240,240,240));   /* handle  */
    fill(cx - 8,  cy - 26, 16, 38, RGB(200,170,90));    /* body    */
    fill(cx - 12, cy - 30, 24, 8,  RGB(120,90,40));     /* ferrule */
    fill(cx - 6,  cy + 12, 12, 10, RGB(220,60,60));     /* tip     */
}
static void icon_clock(int cx, int cy)
{
    frame(cx - 30, cy - 30, 60, 60, 4, RGB(240,240,240));
    fill(cx - 2, cy - 24, 4, 26, RGB(240,240,240));     /* minute hand */
    fill(cx, cy - 2, 20, 4, RGB(240,240,240));          /* hour hand   */
}
static void icon_exit(int cx, int cy)
{
    for (int i = -24; i <= 24; i++) {
        fill(cx + i - 2, cy + i - 2, 5, 5, RGB(240,240,240));
        fill(cx + i - 2, cy - i - 2, 5, 5, RGB(240,240,240));
    }
}

/* a "back" arrow button */
static void back_button(int x, int y, int w, int h)
{
    fill(x, y, w, h, RGB(60,60,72));
    frame(x, y, w, h, 2, RGB(255,255,255));
    fill(x + 26, y + h/2 - 2, 40, 5, RGB(255,255,255));
    for (int i = 0; i < 12; i++)
        fill(x + 14 + i, y + h/2 - i, 3, 2*i + 1, RGB(255,255,255));
}

/* ── tiles ────────────────────────────────────────────────────────── */

typedef struct { int x, y, w, h; uint32_t col; int id; } Tile;

static int hit(const Tile *t, int mx, int my)
{
    return mx >= t->x && mx < t->x + t->w && my >= t->y && my < t->y + t->h;
}

/* ── cursor (a little arrow) ──────────────────────────────────────── */

static void cursor(int x, int y)
{
    for (int i = 0; i < 17; i++) {
        int w = (i < 12) ? i + 2 : (17 - i) * 2 + 2;
        if (w > 11) w = 11;
        fill(x,     y + i, w,     1, RGB(255,255,255));
        fill(x,     y + i, 1,     1, RGB(0,0,0));
        fill(x + w, y + i, 1,     1, RGB(0,0,0));
    }
    fill(x, y + 17, 12, 1, RGB(0,0,0));
}

/* ── main ─────────────────────────────────────────────────────────── */

enum { SCR_DESKTOP, SCR_PAINT, SCR_CLOCK };

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) _exit(1);
    fb    = mmap(0, FB_BYTES, PROT_READ|PROT_WRITE, MAP_SHARED, fd, FB_BASE);
    kbd   = mmap(0, 4096,     PROT_READ|PROT_WRITE, MAP_SHARED, fd, KBD_BASE);
    mouse = mmap(0, 4096,     PROT_READ|PROT_WRITE, MAP_SHARED, fd, MOUSE_BASE);
    if (fb == MAP_FAILED || kbd == MAP_FAILED || mouse == MAP_FAILED) _exit(2);

    int screen = SCR_DESKTOP;
    int cx = FB_W / 2, cy = FB_H / 2;
    int prevbtn = 0;
    uint32_t paintcol = RGB(220,60,60);

    Tile menu[3] = {
        { 180, 250, 200, 230, RGB(190,70,70),  SCR_PAINT },
        { 412, 250, 200, 230, RGB(70,110,190), SCR_CLOCK },
        { 644, 250, 200, 230, RGB(80,80,92),   -1        },   /* exit */
    };
    Tile back = { 24, 22, 96, 50, 0, SCR_DESKTOP };
    uint32_t swatch[6] = {
        RGB(220,60,60), RGB(70,170,90), RGB(70,110,210),
        RGB(230,200,70), RGB(28,28,32), RGB(245,245,245)
    };

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        /* ── input ── */
        int dx  = (int)(int32_t)mouse[1];
        int dy  = (int)(int32_t)mouse[2];
        int btn = (int)mouse[3] & 1;
        cx += dx; cy += dy;
        if (cx < 0) cx = 0; else if (cx > FB_W - 1) cx = FB_W - 1;
        if (cy < 0) cy = 0; else if (cy > FB_H - 1) cy = FB_H - 1;
        int click = btn && !prevbtn;
        prevbtn = btn;

        while (kbd[0]) {
            uint32_t e = kbd[1];
            if (e & 0x100) {
                int k = e & 0xFF;
                if (k == KEY_ESC) {
                    if (screen == SCR_DESKTOP) { fill(0,0,FB_W,FB_H,0); present(); _exit(0); }
                    screen = SCR_DESKTOP;
                } else if (screen == SCR_PAINT && k == KEY_C) {
                    fill(0, 92, FB_W, FB_H - 92, RGB(245,245,245));
                }
            }
        }

        /* ── update + draw ── */
        if (screen == SCR_DESKTOP) {
            gradient(RGB(40,52,88), RGB(14,18,32));
            fill(0, 0, FB_W, 70, RGB(24,28,46));
            fill(0, 70, FB_W, 3, RGB(90,120,210));
            for (int i = 0; i < 3; i++) {
                Tile *t = &menu[i];
                fill(t->x, t->y, t->w, t->h, t->col);
                frame(t->x, t->y, t->w, t->h, 3, RGB(255,255,255));
                int icx = t->x + t->w/2, icy = t->y + t->h/2;
                if (i == 0) icon_paint(icx, icy);
                if (i == 1) icon_clock(icx, icy);
                if (i == 2) icon_exit(icx, icy);
                if (click && hit(t, cx, cy)) {
                    if (t->id == -1) { fill(0,0,FB_W,FB_H,0); present(); _exit(0); }
                    screen = t->id;
                    if (screen == SCR_PAINT)
                        fill(0, 92, FB_W, FB_H - 92, RGB(245,245,245));
                }
            }
            /* live corner clock */
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            int s = (int)(now.tv_sec - t0.tv_sec);
            int mm = (s / 60) % 60, ss = s % 60, s7 = 6, bx = FB_W - 168;
            digit7(bx,        18, s7, mm/10, RGB(120,210,255), RGB(36,40,58));
            digit7(bx + 34,   18, s7, mm%10, RGB(120,210,255), RGB(36,40,58));
            fill(bx + 72, 30, 6, 6, RGB(120,210,255));
            fill(bx + 72, 46, 6, 6, RGB(120,210,255));
            digit7(bx + 88,   18, s7, ss/10, RGB(120,210,255), RGB(36,40,58));
            digit7(bx + 122,  18, s7, ss%10, RGB(120,210,255), RGB(36,40,58));
        }
        else if (screen == SCR_PAINT) {
            fill(0, 0, FB_W, 92, RGB(30,32,40));
            for (int i = 0; i < 6; i++) {
                int sx = 150 + i * 74;
                fill(sx, 21, 58, 50, swatch[i]);
                if (swatch[i] == paintcol)
                    frame(sx - 3, 18, 64, 56, 3, RGB(255,255,255));
                if (click && cx >= sx && cx < sx + 58 && cy >= 21 && cy < 71)
                    paintcol = swatch[i];
            }
            back_button(back.x, back.y, back.w, back.h);
            if (click && hit(&back, cx, cy)) screen = SCR_DESKTOP;
            if (btn && cy > 100)
                fill(cx - 7, cy - 7, 14, 14, paintcol);
        }
        else {  /* SCR_CLOCK */
            gradient(RGB(20,24,40), RGB(4,6,12));
            back_button(back.x, back.y, back.w, back.h);
            if (click && hit(&back, cx, cy)) screen = SCR_DESKTOP;
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            int s  = (int)(now.tv_sec - t0.tv_sec);
            int hh = (s / 3600) % 100, mm = (s / 60) % 60, ss = s % 60;
            int sz = 22, gap = 20;
            int gx = FB_W/2 - (6*(sz*4) + 2*(sz*2) + 6*gap)/2, gy = FB_H/2 - sz*2;
            uint32_t on = RGB(120,230,160), off = RGB(20,42,32);
            int x = gx;
            int two[6] = { hh/10, hh%10, mm/10, mm%10, ss/10, ss%10 };
            for (int i = 0; i < 6; i++) {
                digit7(x, gy, sz, two[i], on, off);
                x += sz*4 + gap;
                if (i == 1 || i == 3) {
                    fill(x + sz/2, gy + sz*2/3, sz, sz, on);
                    fill(x + sz/2, gy + sz*4/3, sz, sz, on);
                    x += sz*2 + gap;
                }
            }
        }

        cursor(cx, cy);
        present();
        usleep(16000);
    }
}
