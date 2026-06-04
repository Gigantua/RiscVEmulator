/* fb_test.c — Tests the framebuffer peripheral.
 * Writes known pixel patterns and signals via UART what was written.
 * The MSTest host then reads the framebuffer memory to verify.
 */
#include <stddef.h>

#define UART_THR     (*(volatile char *)0x10000000)
#define FB_BASE      ((volatile unsigned int *)0x20000000)
#define DISP_WIDTH   (*(volatile unsigned int *)0x20100000)
#define DISP_HEIGHT  (*(volatile unsigned int *)0x20100004)
#define DISP_BPP     (*(volatile unsigned int *)0x20100008)
#define DISP_VSYNC   (*(volatile unsigned int *)0x2010000C)

static void print_str(const char *s)
{
    while (*s) UART_THR = *s++;
}

static void print_uint(unsigned int n)
{
    char buf[12];
    char *p = buf + 11;
    *p = '\0';
    if (n == 0) { *(--p) = '0'; }
    else { while (n) { *(--p) = '0' + (n % 10); n /= 10; } }
    print_str(p);
}

static void check(const char *label, int pass)
{
    print_str(label);
    print_str(pass ? ": OK\n" : ": FAIL\n");
}

void _start(void)
{
    /* Read display info */
    unsigned int w   = DISP_WIDTH;
    unsigned int h   = DISP_HEIGHT;
    unsigned int bpp = DISP_BPP;

    print_str("fb_test\n");
    print_str("width=");  print_uint(w);  print_str("\n");
    print_str("height="); print_uint(h);  print_str("\n");
    print_str("bpp=");    print_uint(bpp); print_str("\n");

    check("width_320", w == 320);
    check("height_200", h == 200);
    check("bpp_32", bpp == 32);

    /* Write pixel at (0,0) = red (0xFFFF0000 in ABGR / little-endian RGBA) */
    /* Our FB is RGBA8888: R=byte0, G=byte1, B=byte2, A=byte3 */
    volatile unsigned char *fb_bytes = (volatile unsigned char *)0x20000000;
    fb_bytes[0] = 0xFF;  /* R */
    fb_bytes[1] = 0x00;  /* G */
    fb_bytes[2] = 0x00;  /* B */
    fb_bytes[3] = 0xFF;  /* A */

    /* Write pixel at (1,0) = green */
    fb_bytes[4] = 0x00;
    fb_bytes[5] = 0xFF;
    fb_bytes[6] = 0x00;
    fb_bytes[7] = 0xFF;

    /* Write pixel at (0,1) = blue — offset = 320*4 = 1280 */
    unsigned int row1 = 320 * 4;
    fb_bytes[row1 + 0] = 0x00;
    fb_bytes[row1 + 1] = 0x00;
    fb_bytes[row1 + 2] = 0xFF;
    fb_bytes[row1 + 3] = 0xFF;

    /* Write a word-aligned pixel at (2,0) using word write = white (0xFFFFFFFF) */
    FB_BASE[2] = 0xFFFFFFFF;  /* pixel at byte offset 8 */

    /* Signal vsync */
    DISP_VSYNC = 1;

    print_str("pixels_written: OK\n");

    /* Exit */
    *(volatile unsigned int *)0x40000000 = 0;   /* HostExit: write exit code -> halt */
    __builtin_unreachable();
}
