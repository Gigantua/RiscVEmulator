/*
 * rvemu-desktop — minimal multi-window compositor for the rvemu Linux guest.
 *
 * Draws an actual desktop:
 *   - 320x200 framebuffer (mmap'd from /dev/mem at 0x85FC0000)
 *   - Several stacked "windows" with title bars
 *   - Mouse cursor tracked via /dev/input/event1
 *   - Keyboard input routed to focused window via /dev/input/event0
 *   - Click on a window to focus it; click on close box (×) to close it
 *   - 8x8 bitmap font for window titles + content
 *
 * No external dependencies — just libc. Statically linked BFLT.
 *
 * Build: same recipe as rvemu-input. See Examples.Linux.Build_RV32i for
 * the cross-compile step that drops this into /usr/bin/rvemu-desktop.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define FB_BASE   0x85C00000UL
#define FB_W      1024
#define FB_H      768
#define FB_BYTES  (FB_W * FB_H * 4)

#define ABGR(r,g,b)  ((uint32_t)(0xFFu << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* Back buffer — we draw the whole frame here, then memcpy to FB in one go.
 * Without this the host SDL viewer's snapshot would catch half-drawn frames
 * and the display flickers heavily as different layers are composited. */
static uint32_t back[FB_W * FB_H];

#define COL_BG        ABGR( 20,  30,  60)
#define COL_TASKBAR   ABGR( 40,  40,  40)
#define COL_WIN_BG    ABGR(230, 230, 230)
#define COL_WIN_TITLE ABGR( 30,  90, 180)
#define COL_WIN_TITLE_INACTIVE ABGR(120, 120, 120)
#define COL_WIN_TXT   ABGR( 30,  30,  30)
#define COL_WIN_BORDER ABGR(  0,   0,   0)
#define COL_CLOSE     ABGR(200,  60,  60)
#define COL_CURSOR    ABGR(255, 255, 255)
#define COL_CURSOR_BORDER ABGR(0, 0, 0)

static volatile uint32_t *fb;

/* ── 8x8 ASCII font (subset 32..127). Each glyph is 8 bytes; bit 0 = leftmost. ── */
static const uint8_t font8x8[96][8] = {
    {0,0,0,0,0,0,0,0},                                         /* ' ' */
    {0x18,0x18,0x18,0x18,0x18,0,0x18,0},                       /* '!' */
    {0x66,0x66,0x66,0,0,0,0,0},                                /* '"' */
    {0x66,0xFF,0x66,0x66,0xFF,0x66,0,0},                       /* '#' */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0},                    /* '$' */
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0},                    /* '%' */
    {0x3C,0x66,0x3C,0x38,0x67,0x66,0x3F,0},                    /* '&' */
    {0x06,0x0C,0x18,0,0,0,0,0},                                /* '\'' */
    {0x0E,0x1C,0x18,0x18,0x18,0x1C,0x0E,0},                    /* '(' */
    {0x70,0x38,0x18,0x18,0x18,0x38,0x70,0},                    /* ')' */
    {0,0x66,0x3C,0xFF,0x3C,0x66,0,0},                          /* '*' */
    {0,0x18,0x18,0x7E,0x18,0x18,0,0},                          /* '+' */
    {0,0,0,0,0,0x18,0x18,0x30},                                /* ',' */
    {0,0,0,0x7E,0,0,0,0},                                      /* '-' */
    {0,0,0,0,0,0x18,0x18,0},                                   /* '.' */
    {0,0x03,0x06,0x0C,0x18,0x30,0x60,0},                       /* '/' */
    {0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0},                    /* '0' */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},                    /* '1' */
    {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0},                    /* '2' */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},                    /* '3' */
    {0x0E,0x1E,0x36,0x66,0x7F,0x06,0x0F,0},                    /* '4' */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},                    /* '5' */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},                    /* '6' */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},                    /* '7' */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},                    /* '8' */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},                    /* '9' */
    {0,0x18,0x18,0,0,0x18,0x18,0},                             /* ':' */
    {0,0x18,0x18,0,0,0x18,0x18,0x30},                          /* ';' */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0},                    /* '<' */
    {0,0,0x7E,0,0x7E,0,0,0},                                   /* '=' */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0},                    /* '>' */
    {0x3C,0x66,0x06,0x0C,0x18,0,0x18,0},                       /* '?' */
    {0x3C,0x66,0x6E,0x6E,0x60,0x66,0x3C,0},                    /* '@' */
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},                    /* 'A' */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},                    /* 'B' */
    {0x1C,0x36,0x60,0x60,0x60,0x36,0x1C,0},                    /* 'C' */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0},                    /* 'D' */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0},                    /* 'E' */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0},                    /* 'F' */
    {0x1C,0x36,0x60,0x6E,0x66,0x36,0x1C,0},                    /* 'G' */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},                    /* 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0},                    /* 'I' */
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0},                    /* 'J' */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0},                    /* 'K' */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},                    /* 'L' */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0},                    /* 'M' */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0},                    /* 'N' */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},                    /* 'O' */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},                    /* 'P' */
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0},                    /* 'Q' */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0},                    /* 'R' */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0},                    /* 'S' */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},                    /* 'T' */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},                    /* 'U' */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0},                    /* 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0},                    /* 'W' */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},                    /* 'X' */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0},                    /* 'Y' */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0},                    /* 'Z' */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0},                    /* '[' */
    {0,0x60,0x30,0x18,0x0C,0x06,0x03,0},                       /* '\\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0},                    /* ']' */
    {0x18,0x3C,0x66,0,0,0,0,0},                                /* '^' */
    {0,0,0,0,0,0,0,0xFF},                                      /* '_' */
    {0x30,0x18,0x0C,0,0,0,0,0},                                /* '`' */
    {0,0,0x3C,0x06,0x3E,0x66,0x3E,0},                          /* 'a' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0},                    /* 'b' */
    {0,0,0x3C,0x66,0x60,0x66,0x3C,0},                          /* 'c' */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0},                    /* 'd' */
    {0,0,0x3C,0x66,0x7E,0x60,0x3C,0},                          /* 'e' */
    {0x0E,0x18,0x3E,0x18,0x18,0x18,0x18,0},                    /* 'f' */
    {0,0,0x3E,0x66,0x66,0x3E,0x06,0x3C},                       /* 'g' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0},                    /* 'h' */
    {0x18,0,0x38,0x18,0x18,0x18,0x3C,0},                       /* 'i' */
    {0x06,0,0x06,0x06,0x06,0x06,0x66,0x3C},                    /* 'j' */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0},                    /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0},                    /* 'l' */
    {0,0,0x66,0x7F,0x7F,0x6B,0x63,0},                          /* 'm' */
    {0,0,0x7C,0x66,0x66,0x66,0x66,0},                          /* 'n' */
    {0,0,0x3C,0x66,0x66,0x66,0x3C,0},                          /* 'o' */
    {0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},                       /* 'p' */
    {0,0,0x3E,0x66,0x66,0x3E,0x06,0x06},                       /* 'q' */
    {0,0,0x7C,0x66,0x60,0x60,0x60,0},                          /* 'r' */
    {0,0,0x3E,0x60,0x3C,0x06,0x7C,0},                          /* 's' */
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0},                    /* 't' */
    {0,0,0x66,0x66,0x66,0x66,0x3E,0},                          /* 'u' */
    {0,0,0x66,0x66,0x66,0x3C,0x18,0},                          /* 'v' */
    {0,0,0x63,0x6B,0x7F,0x3E,0x36,0},                          /* 'w' */
    {0,0,0x66,0x3C,0x18,0x3C,0x66,0},                          /* 'x' */
    {0,0,0x66,0x66,0x66,0x3E,0x06,0x3C},                       /* 'y' */
    {0,0,0x7E,0x0C,0x18,0x30,0x7E,0},                          /* 'z' */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0},                    /* '{' */
    {0x18,0x18,0x18,0,0x18,0x18,0x18,0},                       /* '|' */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0},                    /* '}' */
    {0x76,0xDC,0,0,0,0,0,0},                                   /* '~' */
    {0,0,0,0,0,0,0,0},                                         /* DEL */
};

static inline void px(int x, int y, uint32_t c)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H) back[y * FB_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > FB_W) x1 = FB_W;
    if (y1 > FB_H) y1 = FB_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            back[yy * FB_W + xx] = c;
}

static void present(void)
{
    /* Single bulk copy from back buffer to the live FB. The SDL viewer's
     * snapshot grabs whole rows at once, so this is effectively tear-free. */
    memcpy((void *)fb, back, FB_BYTES);
}

static void draw_char(int x, int y, char ch, uint32_t fg)
{
    if ((unsigned)(ch - 32) >= 96) ch = '?';
    const uint8_t *g = font8x8[ch - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                px(x + col, y + row, fg);
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t fg)
{
    while (*s) {
        draw_char(x, y, *s, fg);
        x += 8;
        s++;
    }
}

/* ── Window list ────────────────────────────────────────────────────────── */

#define MAX_WINDOWS 8

struct Win {
    int  x, y, w, h;
    int  alive;
    char title[32];
    char content[8][32];   /* up to 8 lines of text content */
    int  lines;
};

static struct Win wins[MAX_WINDOWS];
static int focused = -1;

static void win_draw(int i)
{
    const struct Win *w = &wins[i];
    if (!w->alive) return;
    uint32_t tcol = (i == focused) ? COL_WIN_TITLE : COL_WIN_TITLE_INACTIVE;

    /* Border */
    fill_rect(w->x, w->y, w->w, w->h, COL_WIN_BORDER);
    /* Inner background */
    fill_rect(w->x + 1, w->y + 11, w->w - 2, w->h - 12, COL_WIN_BG);
    /* Title bar */
    fill_rect(w->x + 1, w->y + 1, w->w - 2, 10, tcol);
    /* Close box */
    fill_rect(w->x + w->w - 10, w->y + 1, 9, 10, COL_CLOSE);
    draw_char(w->x + w->w - 9, w->y + 2, 'x', ABGR(255,255,255));
    /* Title */
    draw_text(w->x + 4, w->y + 2, w->title, ABGR(255, 255, 255));
    /* Content */
    for (int l = 0; l < w->lines && l < 8; l++)
        draw_text(w->x + 4, w->y + 14 + l * 9, w->content[l], COL_WIN_TXT);
}

static void draw_cursor(int x, int y)
{
    /* 6x10 arrow cursor (filled triangle). Slight border for visibility. */
    static const uint8_t mask[10] = { 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xF8, 0xC0, 0x80 };
    for (int r = 0; r < 10; r++) {
        uint8_t m = mask[r];
        for (int c = 0; c < 8; c++) {
            if (m & (0x80 >> c)) {
                px(x + c, y + r, COL_CURSOR);
            }
        }
    }
    /* Black outline along the right edge */
    for (int r = 0; r < 10; r++) {
        for (int c = 7; c >= 0; c--) {
            if (mask[r] & (0x80 >> c)) {
                px(x + c + 1, y + r, COL_CURSOR_BORDER);
                break;
            }
        }
    }
}

static void taskbar_draw(int mx, int my)
{
    fill_rect(0, FB_H - 12, FB_W, 12, COL_TASKBAR);
    /* List window titles */
    int x = 4;
    for (int i = 0; i < MAX_WINDOWS && x < FB_W - 60; i++) {
        if (!wins[i].alive) continue;
        uint32_t c = (i == focused) ? ABGR(255, 220, 100) : ABGR(180, 180, 180);
        draw_text(x, FB_H - 10, wins[i].title, c);
        x += (int)strlen(wins[i].title) * 8 + 8;
    }
    /* Time on the right */
    char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        draw_text(FB_W - 64, FB_H - 10, buf, ABGR(180, 220, 255));
    }
    /* Coordinate hint */
    snprintf(buf, sizeof(buf), "%d,%d", mx, my);
    draw_text(FB_W - 130, FB_H - 10, buf, ABGR(150, 150, 150));
}

static void compose(int mx, int my)
{
    fill_rect(0, 0, FB_W, FB_H - 12, COL_BG);
    /* Wallpaper checkerboard */
    for (int y = 0; y < FB_H - 12; y += 16) {
        for (int x = 0; x < FB_W; x += 16) {
            if (((x ^ y) & 16) == 0) {
                fill_rect(x, y, 16, 16, ABGR(25, 35, 70));
            }
        }
    }
    /* Stack windows back-to-front (focused on top) */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].alive) continue;
            if (pass == 0 && i == focused) continue;
            if (pass == 1 && i != focused) continue;
            win_draw(i);
        }
    }
    taskbar_draw(mx, my);
    draw_cursor(mx, my);
}

/* ── Mouse / kbd state ──────────────────────────────────────────────────── */

static int mx = FB_W / 2, my = (FB_H - 12) / 2;
static int btn_down = 0;
static int drag_idx = -1, drag_dx = 0, drag_dy = 0;

static int hit_test(int x, int y, int *close_hit)
{
    *close_hit = 0;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (!wins[i].alive) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w &&
            y >= wins[i].y && y < wins[i].y + wins[i].h)
        {
            if (x >= wins[i].x + wins[i].w - 10 && x < wins[i].x + wins[i].w - 1 &&
                y >= wins[i].y + 1 && y < wins[i].y + 11)
                *close_hit = 1;
            return i;
        }
    }
    return -1;
}

static void seed_windows(void)
{
    strcpy(wins[0].title, "Welcome");
    wins[0].x = 14;  wins[0].y = 18; wins[0].w = 188; wins[0].h = 84; wins[0].alive = 1;
    strcpy(wins[0].content[0], "Hello rvemu!");
    strcpy(wins[0].content[1], "drag titlebar to");
    strcpy(wins[0].content[2], "move; click X");
    strcpy(wins[0].content[3], "to close.");
    wins[0].lines = 4;

    strcpy(wins[1].title, "Stats");
    wins[1].x = 150; wins[1].y = 55; wins[1].w = 156; wins[1].h = 70; wins[1].alive = 1;
    strcpy(wins[1].content[0], "uname: rvemu");
    strcpy(wins[1].content[1], "arch: rv32i nommu");
    strcpy(wins[1].content[2], "ram: 96 mb");
    strcpy(wins[1].content[3], "up: ?");
    wins[1].lines = 4;

    strcpy(wins[2].title, "Clock");
    wins[2].x = 60;  wins[2].y = 102; wins[2].w = 120; wins[2].h = 40; wins[2].alive = 1;
    strcpy(wins[2].content[0], "");
    wins[2].lines = 1;

    focused = 0;
}

/* Refresh dynamic content (clock, uptime). Called once per frame. */
static void refresh_dynamic_windows(void)
{
    /* Clock: HH:MM:SS UTC */
    if (wins[2].alive) {
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        if (tm) {
            snprintf(wins[2].content[0], sizeof(wins[2].content[0]),
                     " %02d:%02d:%02d UTC", tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
    }

    /* Uptime from /proc/uptime (first whitespace-separated token = seconds float). */
    if (wins[1].alive) {
        int fd = open("/proc/uptime", O_RDONLY);
        if (fd >= 0) {
            char buf[64] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                /* Stop at first space — we only want the uptime field. */
                for (ssize_t i = 0; i < n; i++) if (buf[i] == ' ') { buf[i] = 0; break; }
                /* Parse as seconds, format as MM:SS. */
                int secs = (int)atoi(buf);
                int mm = secs / 60, ss = secs % 60;
                snprintf(wins[1].content[3], sizeof(wins[1].content[3]),
                         "up: %d:%02d", mm, ss);
            }
        }
    }
}

int main(void)
{
    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("/dev/mem"); return 1; }
    void *p = mmap(NULL, FB_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, mem, FB_BASE);
    if (p == MAP_FAILED) { perror("mmap fb"); return 1; }
    fb = (volatile uint32_t *)p;

    int kbd_fd   = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    int mouse_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0 || mouse_fd < 0) {
        const char m[] = "rvemu-desktop: /dev/input/event0+1 missing (rvemu-input not running?)\n";
        (void)write(2, m, sizeof(m) - 1);
        return 1;
    }

    seed_windows();
    compose(mx, my);

    struct input_event ev;
    for (;;) {
        /* Mouse events */
        while (read(mouse_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) mx += ev.value;
                if (ev.code == REL_Y) my += ev.value;
                if (mx < 0)        mx = 0;
                if (mx >= FB_W)    mx = FB_W - 1;
                if (my < 0)        my = 0;
                if (my >= FB_H)    my = FB_H - 1;
                if (drag_idx >= 0) {
                    wins[drag_idx].x = mx - drag_dx;
                    wins[drag_idx].y = my - drag_dy;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                if (ev.value == 1) {  /* press */
                    int close_hit = 0;
                    int idx = hit_test(mx, my, &close_hit);
                    if (idx >= 0) {
                        if (close_hit) {
                            wins[idx].alive = 0;
                            if (focused == idx) focused = -1;
                        } else {
                            focused = idx;
                            /* Drag if in title bar */
                            if (my < wins[idx].y + 11) {
                                drag_idx = idx;
                                drag_dx  = mx - wins[idx].x;
                                drag_dy  = my - wins[idx].y;
                            }
                        }
                    }
                    btn_down = 1;
                } else {  /* release */
                    btn_down = 0;
                    drag_idx = -1;
                }
            }
        }
        /* Keyboard — drop into focused window's first line (for fun) */
        while (read(kbd_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1 && focused >= 0) {
                /* very rough scancode→ASCII for letters/digits/space */
                static const char map[128] = {
                    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
                    [16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
                    [30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
                    [44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',
                    [57]=' ',
                };
                char c = (ev.code < 128) ? map[ev.code] : 0;
                if (c) {
                    size_t n = strlen(wins[focused].content[0]);
                    if (n + 1 < sizeof(wins[focused].content[0])) {
                        wins[focused].content[0][n]     = c;
                        wins[focused].content[0][n + 1] = 0;
                        if (wins[focused].lines == 0) wins[focused].lines = 1;
                    }
                }
            }
        }
        refresh_dynamic_windows();
        compose(mx, my);
        present();

        /* ~30 fps */
        struct timespec sleep = { 0, 33 * 1000 * 1000 };
        nanosleep(&sleep, NULL);
    }
}
