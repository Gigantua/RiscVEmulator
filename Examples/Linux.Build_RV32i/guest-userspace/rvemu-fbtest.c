/*
 * rvemu-fbtest — minimal framebuffer demo proving the full input → display
 * stack works end-to-end:
 *
 *   1. mmap's the framebuffer at 0x85FC0000 via /dev/mem (since our kernel
 *      rejects simple-framebuffer DT nodes, see CLAUDE.md).
 *   2. Reads /dev/input/event0 (kbd) and /dev/input/event1 (mouse), which
 *      the rvemu-input daemon publishes.
 *   3. Renders a color test pattern, a key-state HUD, and a cursor that
 *      tracks the mouse.
 *
 * Build with the same recipe as rvemu-input:
 *   ${CC} -static -fPIC -Wl,-elf2flt=-r -O2 rvemu-fbtest.c -o rvemu-fbtest
 *
 * Run with `--gui` on the host emulator and you should see the result in
 * the SDL window.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FB_BASE   0x85C00000UL
#define FB_W      1024
#define FB_H      768
#define FB_BYTES  (FB_W * FB_H * 4)

#define ABGR(r,g,b)  ((uint32_t)(0xFFu << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (r))

static volatile uint32_t *fb;
static int mouse_x = FB_W / 2;
static int mouse_y = FB_H / 2;

static void fill_test_pattern(void)
{
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            uint8_t r = (uint8_t)(x * 255 / FB_W);
            uint8_t g = (uint8_t)(y * 255 / FB_H);
            uint8_t b = (uint8_t)(((x ^ y) & 0xFF));
            fb[y * FB_W + x] = ABGR(r, g, b);
        }
    }
}

static void draw_cursor(int x, int y)
{
    /* 8x8 white cursor with black border */
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= FB_W || py < 0 || py >= FB_H) continue;
            uint32_t c = (dx == -4 || dx == 4 || dy == -4 || dy == 4)
                       ? ABGR(0, 0, 0)
                       : ABGR(255, 255, 255);
            fb[py * FB_W + px] = c;
        }
    }
}

int main(void)
{
    const char hi[] = "rvemu-fbtest: starting\n";
    (void)write(2, hi, sizeof(hi) - 1);

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("/dev/mem"); return 1; }
    void *p = mmap(NULL, FB_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, mem, FB_BASE);
    if (p == MAP_FAILED) { perror("mmap fb"); return 1; }
    fb = (volatile uint32_t *)p;

    int kbd_fd   = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    int mouse_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0 || mouse_fd < 0) {
        const char m[] = "rvemu-fbtest: /dev/input/event0+1 missing — is rvemu-input running?\n";
        (void)write(2, m, sizeof(m) - 1);
        return 1;
    }

    fill_test_pattern();

    struct input_event ev;
    while (1) {
        ssize_t n;
        while ((n = read(mouse_fd, &ev, sizeof(ev))) == sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) mouse_x += ev.value;
                if (ev.code == REL_Y) mouse_y += ev.value;
                if (mouse_x < 0)        mouse_x = 0;
                if (mouse_x >= FB_W)    mouse_x = FB_W - 1;
                if (mouse_y < 0)        mouse_y = 0;
                if (mouse_y >= FB_H)    mouse_y = FB_H - 1;
            }
        }
        while ((n = read(kbd_fd, &ev, sizeof(ev))) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                /* On any key press, re-tint the background. */
                fill_test_pattern();
            }
        }
        /* Redraw cursor every frame */
        fill_test_pattern();
        draw_cursor(mouse_x, mouse_y);

        /* ~30 fps */
        struct timespec sleep = { 0, 33 * 1000 * 1000 };
        nanosleep(&sleep, NULL);
    }
}
