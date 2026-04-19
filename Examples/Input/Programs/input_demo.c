/* input_demo.c — Keyboard + mouse input demo for the RV32I emulator.
 *
 * Displays a cursor on screen controlled by the mouse, and shows
 * the last key pressed as a colored character. Demonstrates all
 * MMIO input peripherals.
 *
 * Compile:
 *   clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
 *         -nostdlib -nostartfiles -O3 -fno-builtin -fuse-ld=lld \
 *         -Wl,-Tlinker.ld input_demo.c runtime.o -o input_demo.elf
 */

#include "libc.h"

/* Keyboard */
#define KB_STATUS    (*(volatile unsigned int *)0x10001000)
#define KB_DATA      (*(volatile unsigned int *)0x10001004)

/* Mouse */
#define MOUSE_STATUS (*(volatile unsigned int *)0x10002000)
#define MOUSE_DX     (*(volatile int *)0x10002004)
#define MOUSE_DY     (*(volatile int *)0x10002008)
#define MOUSE_BTN    (*(volatile unsigned int *)0x1000200C)

/* Framebuffer */
#define FB_BASE      ((volatile unsigned int *)0x20000000)
#define WIDTH        320
#define HEIGHT       200

/* RTC */
#define RTC_MS_LO    (*(volatile unsigned int *)0x10003008)

static unsigned int pack_rgba(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned int)r | ((unsigned int)g << 8) | ((unsigned int)b << 16) | (0xFFu << 24);
}

/* Back-buffer:draw here, then copy to FB_BASE in one shot to avoid tearing */
static unsigned int backbuf[WIDTH * HEIGHT];

static void flip(void)
{
    for (int i = 0; i < WIDTH * HEIGHT; i++)
        FB_BASE[i] = backbuf[i];
}

static void bb_clear(unsigned int color)
{
    for (int i = 0; i < WIDTH * HEIGHT; i++)
        backbuf[i] = color;
}

static void bb_rect(int x, int y, int w, int h, unsigned int color)
{
    for (int dy = 0; dy < h; dy++)
    {
        int py = y + dy;
        if (py < 0 || py >= HEIGHT) continue;
        for (int dx = 0; dx < w; dx++)
        {
            int px = x + dx;
            if (px < 0 || px >= WIDTH) continue;
            backbuf[py * WIDTH + px] = color;
        }
    }
}

static void bb_cursor(int cx, int cy, unsigned int color)
{
    for (int d = -5; d <= 5; d++)
    {
        int px = cx + d;
        if (px >= 0 && px < WIDTH && cy >= 0 && cy < HEIGHT)
            backbuf[cy * WIDTH + px] = color;
        int py = cy + d;
        if (py >= 0 && py < HEIGHT && cx >= 0 && cx < WIDTH)
            backbuf[py * WIDTH + cx] = color;
    }
}

#define FRAME_MS    16  /* ~60 fps cap */

void _start(void)
{
    printf("Input demo: move mouse, press keys\n");
    printf("  Mouse controls cursor, keys light up on screen\n");
    printf("  Press F10 to release mouse grab\n");

    int cursorX = WIDTH / 2;
    int cursorY = HEIGHT / 2;
    unsigned char lastKey = 0;
    unsigned int buttons = 0;
    unsigned int lastFrameMs = RTC_MS_LO;

    while (1)
    {
        /* Poll keyboard — drain every iteration for responsiveness */
        while (KB_STATUS & 1)
        {
            unsigned int data = KB_DATA;
            unsigned char keycode = (unsigned char)(data & 0xFF);
            int pressed = (data & 0x100) ? 1 : 0;

            if (pressed)
            {
                lastKey = keycode;
                printf("KEY: 0x%02X DOWN\n", keycode);
            }
        }

        /* Poll mouse — accumulate between frames */
        if (MOUSE_STATUS & 1)
        {
            int dx = MOUSE_DX / 2;
            int dy = MOUSE_DY / 2;
            cursorX += dx;
            cursorY += dy;

            if (cursorX < 0) cursorX = 0;
            if (cursorX >= WIDTH) cursorX = WIDTH - 1;
            if (cursorY < 0) cursorY = 0;
            if (cursorY >= HEIGHT) cursorY = HEIGHT - 1;
        }
        buttons = MOUSE_BTN;

        /* Frame-rate limit: only redraw every FRAME_MS milliseconds */
        unsigned int now = RTC_MS_LO;
        if (now - lastFrameMs < FRAME_MS)
            continue;
        lastFrameMs = now;

        /* Draw into back-buffer */
        bb_clear(pack_rgba(20, 20, 40));

        /* Status bar at top */
        bb_rect(0, 0, WIDTH, 12, pack_rgba(60, 60, 80));

        /* Show last key as a colored block in the bar */
        if (lastKey > 0)
        {
            bb_rect(4, 2, 8, 8, pack_rgba(255, 200, 50));

            /* If printable, show a bigger version in center */
            if (lastKey >= 0x20 && lastKey < 0x7F)
            {
                bb_rect(WIDTH / 2 - 20, HEIGHT / 2 - 20, 40, 40,
                        pack_rgba(200, 100, 50));
            }
        }

        /* Show mouse buttons as colored rectangles */
        if (buttons & 1)  bb_rect(WIDTH - 40, 2, 10, 8, pack_rgba(255, 0, 0));
        if (buttons & 2)  bb_rect(WIDTH - 28, 2, 10, 8, pack_rgba(0, 255, 0));
        if (buttons & 4)  bb_rect(WIDTH - 16, 2, 10, 8, pack_rgba(0, 0, 255));

        /* Draw cursor */
        unsigned int cursorColor = (buttons & 1) ? pack_rgba(255, 50, 50)
                                                 : pack_rgba(255, 255, 255);
        bb_cursor(cursorX, cursorY, cursorColor);

        /* Single atomic copy to visible framebuffer — no tearing */
        flip();
    }
}
