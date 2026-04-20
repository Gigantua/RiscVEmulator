/* video_demo.c — Framebuffer demo for the RV32I emulator.
 *
 * Draws animated color patterns to the 320×200 RGBA8888 framebuffer.
 * Cycles through: gradient bars, a moving plasma-like pattern, and
 * a bouncing square — all using only integer math (pure RV32I).
 *
 * Compile:
 *   clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
 *         -nostdlib -nostartfiles -O3 -fno-builtin -fuse-ld=lld \
 *         -Wl,-Tlinker.ld video_demo.c runtime.o -o video_demo.elf
 */

#include "libc.h"

#define RTC_MS_LO    (*(volatile unsigned int *)0x10003008)
#define DISP_VSYNC   (*(volatile unsigned int *)0x2010000C)
#define DISP_FB_ADDR (*(volatile unsigned int *)0x2010001C)

#define WIDTH  320
#define HEIGHT 200

static unsigned int framebuf[WIDTH * HEIGHT];

static unsigned int pack_rgba(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned int)r | ((unsigned int)g << 8) | ((unsigned int)b << 16) | (0xFFu << 24);
}

/* ── Pattern 1: Vertical rainbow gradient bars ─────────────────── */
static void draw_gradient(int frame)
{
    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            unsigned char r = (unsigned char)((x + frame) & 0xFF);
            unsigned char g = (unsigned char)((y * 2) & 0xFF);
            unsigned char b = (unsigned char)(((x + y + frame) >> 1) & 0xFF);
            framebuf[y * WIDTH + x] = pack_rgba(r, g, b);
        }
    }
}

/* ── Pattern 2: XOR pattern (classic demoscene) ────────────────── */
static void draw_xor(int frame)
{
    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            unsigned char v = (unsigned char)((x ^ y ^ frame) & 0xFF);
            framebuf[y * WIDTH + x] = pack_rgba(v, v >> 1, v << 1);
        }
    }
}

/* ── Pattern 3: Bouncing filled rectangle ──────────────────────── */
static void draw_bouncer(int frame)
{
    int bx = frame % (WIDTH * 2);
    int by = (frame * 3 / 4) % (HEIGHT * 2);
    if (bx >= WIDTH)  bx = WIDTH * 2 - bx;
    if (by >= HEIGHT) by = HEIGHT * 2 - by;
    int bw = 40, bh = 30;

    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            if (x >= bx && x < bx + bw && y >= by && y < by + bh)
                framebuf[y * WIDTH + x] = pack_rgba(255, 255, 0);
            else
                framebuf[y * WIDTH + x] = pack_rgba(0, 0, (unsigned char)(y & 0x3F));
        }
    }
}

void _start(void)
{
    printf("Video demo: starting\n");

    DISP_FB_ADDR = (unsigned int)framebuf;

    int frame = 0;
    unsigned int lastMs = RTC_MS_LO;

    while (1)
    {
        /* Cycle through patterns every ~3 seconds */
        int pattern = (frame / 90) % 3;

        switch (pattern)
        {
            case 0: draw_gradient(frame); break;
            case 1: draw_xor(frame);      break;
            case 2: draw_bouncer(frame);   break;
        }

        DISP_VSYNC = 1;
        frame++;

        /* Print FPS every 1000ms */
        unsigned int now = RTC_MS_LO;
        if (now - lastMs >= 1000)
        {
            printf("frame: %d\n", frame);
            lastMs = now;
        }
    }
}
