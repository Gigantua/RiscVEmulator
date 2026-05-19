/* mmio_devices_test.c — Comprehensive MMIO peripheral tests.
 * Tests all memory-mapped I/O devices from the guest (RV32I) side.
 * Host injects keyboard/mouse events before run; guest reads and verifies.
 * Guest writes pixels/audio/display config; host verifies after run.
 */
#include <stddef.h>

/* ── UART ─────────────────────────────────────────── */
#define UART_THR  (*(volatile char *)0x10000000)
#define UART_RBR  (*(volatile unsigned char *)0x10000000)
#define UART_LSR  (*(volatile unsigned char *)0x10000005)

/* ── Keyboard ─────────────────────────────────────── */
#define KBD_STATUS   (*(volatile unsigned int *)0x10001000)
#define KBD_DATA     (*(volatile unsigned int *)0x10001004)
#define KBD_MODIFIER (*(volatile unsigned int *)0x10001008)

/* ── Mouse ────────────────────────────────────────── */
#define MOUSE_STATUS  (*(volatile unsigned int *)0x10002000)
#define MOUSE_DX      (*(volatile unsigned int *)0x10002004)
#define MOUSE_DY      (*(volatile unsigned int *)0x10002008)
#define MOUSE_BUTTONS (*(volatile unsigned int *)0x1000200C)

/* ── Real-Time Clock ──────────────────────────────── */
#define RTC_US_LO    (*(volatile unsigned int *)0x10003000)
#define RTC_US_HI    (*(volatile unsigned int *)0x10003004)
#define RTC_MS_LO    (*(volatile unsigned int *)0x10003008)
#define RTC_MS_HI    (*(volatile unsigned int *)0x1000300C)
#define RTC_EPOCH_LO (*(volatile unsigned int *)0x10003010)
#define RTC_EPOCH_HI (*(volatile unsigned int *)0x10003014)

/* ── CLINT Timer ──────────────────────────────────── */
#define MTIME_LO  (*(volatile unsigned int *)0x0200BFF8)
#define MTIME_HI  (*(volatile unsigned int *)0x0200BFFC)

/* ── Framebuffer ──────────────────────────────────── */
#define FB_BYTES  ((volatile unsigned char *)0x20000000)
#define FB_WORDS  ((volatile unsigned int  *)0x20000000)

/* ── Display Control ──────────────────────────────── */
#define DISP_WIDTH    (*(volatile unsigned int *)0x20100000)
#define DISP_HEIGHT   (*(volatile unsigned int *)0x20100004)
#define DISP_BPP      (*(volatile unsigned int *)0x20100008)
#define DISP_VSYNC    (*(volatile unsigned int *)0x2010000C)
#define DISP_PAL_IDX  (*(volatile unsigned int *)0x20100010)
#define DISP_PAL_DATA (*(volatile unsigned int *)0x20100014)
#define DISP_MODE     (*(volatile unsigned int *)0x20100018)

/* ── Audio ────────────────────────────────────────── */
#define AUDIO_BUF     ((volatile unsigned char *)0x30000000)
#define AUDIO_CTRL    (*(volatile unsigned int *)0x30100000)
#define AUDIO_STATUS  (*(volatile unsigned int *)0x30100004)
#define AUDIO_SRATE   (*(volatile unsigned int *)0x30100008)
#define AUDIO_CHAN    (*(volatile unsigned int *)0x3010000C)
#define AUDIO_BITS    (*(volatile unsigned int *)0x30100010)
#define AUDIO_BSTART  (*(volatile unsigned int *)0x30100014)
#define AUDIO_BLEN    (*(volatile unsigned int *)0x30100018)
#define AUDIO_POS     (*(volatile unsigned int *)0x3010001C)

/* ── Helpers ──────────────────────────────────────── */
static void put(const char *s) { while (*s) UART_THR = *s++; }
static void put_uint(unsigned int n) {
    char buf[12]; char *p = buf + 11; *p = '\0';
    if (n == 0) { *(--p) = '0'; }
    else { while (n) { *(--p) = '0' + (n % 10); n /= 10; } }
    put(p);
}
static void put_int(int n) {
    if (n < 0) { UART_THR = '-'; n = -n; }
    put_uint((unsigned)n);
}
static void ok(const char *label, int pass) {
    put(label); put(pass ? ": OK\n" : ": FAIL\n");
}

static void exit_emu(int code) {
    __asm__ volatile("mv a0, %0\n li a7, 93\n ecall\n" :: "r"(code));
    __builtin_unreachable();
}

/* ═════════════════════════════════════════════════════
 * TEST SECTIONS
 * ═════════════════════════════════════════════════════ */

/* ── 1. UART loopback & LSR ───────────────────────── */
static void test_uart(void)
{
    put("[uart]\n");

    /* LSR bit 5 (TX empty) should always be set */
    unsigned char lsr = UART_LSR;
    ok("uart_tx_ready", (lsr & 0x20) != 0);

    /* Write a known character */
    UART_THR = '!';
    put("\n");

    /* LSR bit 0 (RX ready) — we have input injected, so it should be set */
    lsr = UART_LSR;
    ok("uart_rx_has_data", (lsr & 0x01) != 0);
}

/* ── 2. Keyboard ──────────────────────────────────── */
static void test_keyboard(void)
{
    put("[keyboard]\n");

    /* Expect 5 events injected by host:
     *   'w'(0x77) press, 'a'(0x61) press, 's'(0x73) press,
     *   'd'(0x64) press, 'w'(0x77) release
     * Modifiers: shift=1, ctrl=0, alt=0
     */
    ok("kbd_has_events", KBD_STATUS != 0);

    unsigned int e1 = KBD_DATA;  /* w press  = 0x77 | 0x100 = 0x177 */
    ok("kbd_w_press", e1 == 0x177);

    unsigned int e2 = KBD_DATA;  /* a press  = 0x61 | 0x100 = 0x161 */
    ok("kbd_a_press", e2 == 0x161);

    unsigned int e3 = KBD_DATA;  /* s press  = 0x73 | 0x100 = 0x173 */
    ok("kbd_s_press", e3 == 0x173);

    unsigned int e4 = KBD_DATA;  /* d press  = 0x64 | 0x100 = 0x164 */
    ok("kbd_d_press", e4 == 0x164);

    unsigned int e5 = KBD_DATA;  /* w release = 0x77 | 0x000 = 0x077 */
    ok("kbd_w_release", e5 == 0x077);

    /* FIFO should now be empty */
    ok("kbd_fifo_empty", KBD_STATUS == 0);

    /* Reading empty FIFO returns 0 */
    ok("kbd_empty_read_zero", KBD_DATA == 0);

    /* Modifier flags (shift was set by host) */
    unsigned int mod = KBD_MODIFIER;
    ok("kbd_mod_shift", (mod & 1) != 0);
    ok("kbd_mod_no_ctrl", (mod & 2) == 0);
    ok("kbd_mod_no_alt", (mod & 4) == 0);
}

/* ── 3. Mouse ─────────────────────────────────────── */
static void test_mouse(void)
{
    put("[mouse]\n");

    /* Expect: dx=+42, dy=-17, left+right buttons pressed */
    ok("mouse_has_data", MOUSE_STATUS != 0);

    int dx = (int)MOUSE_DX;
    int dy = (int)MOUSE_DY;
    unsigned int btn = MOUSE_BUTTONS;

    ok("mouse_dx_42", dx == 42);
    ok("mouse_dy_neg17", dy == -17);
    ok("mouse_left_btn", (btn & 1) != 0);
    ok("mouse_right_btn", (btn & 2) != 0);
    ok("mouse_no_middle", (btn & 4) == 0);

    /* After read, dx/dy reset to 0 */
    ok("mouse_dx_reset", (int)MOUSE_DX == 0);
    ok("mouse_dy_reset", (int)MOUSE_DY == 0);

    /* Buttons persist until host clears them */
    ok("mouse_btn_persists", MOUSE_BUTTONS == btn);

    /* STATUS should show no new motion data (buttons still set though) */
    /* After dx/dy cleared, hasData depends on buttons */
    ok("mouse_status_btn", MOUSE_STATUS != 0);
}

/* ── 4. Real-Time Clock ───────────────────────────── */
static void test_rtc(void)
{
    put("[rtc]\n");

    /* Read microsecond timer */
    unsigned int us_lo1 = RTC_US_LO;
    unsigned int us_hi1 = RTC_US_HI;

    /* Microsecond value should be non-zero (test runs after some CPU work) */
    ok("rtc_us_nonzero", us_lo1 > 0 || us_hi1 > 0);

    /* Do some work and read again — should advance */
    volatile unsigned int dummy = 0;
    for (int i = 0; i < 1000; i++) dummy += i;
    (void)dummy;

    unsigned int us_lo2 = RTC_US_LO;
    ok("rtc_us_advances", us_lo2 > us_lo1 || RTC_US_HI > us_hi1);

    /* Millisecond register */
    unsigned int ms = RTC_MS_LO;
    /* ms might be 0 if test runs very fast, just check it's readable */
    put("rtc_ms="); put_uint(ms); put("\n");
    ok("rtc_ms_readable", 1); /* always passes — just that the read didn't fault */

    /* Epoch should be > 1700000000 (somewhere after ~2023) */
    unsigned int epoch = RTC_EPOCH_LO;
    put("rtc_epoch="); put_uint(epoch); put("\n");
    ok("rtc_epoch_plausible", epoch > 1700000000u);

    /* High part of epoch should be 0 for dates before ~2106 */
    ok("rtc_epoch_hi_zero", RTC_EPOCH_HI == 0);

    /* RTC should be independent from CLINT timer */
    unsigned int mtime = MTIME_LO;
    ok("rtc_vs_clint_differ", us_lo2 != mtime);
}

/* ── 5. Framebuffer — yellow pixels ───────────────── */
static void test_framebuffer(void)
{
    put("[framebuffer]\n");

    /* Verify display dimensions */
    ok("fb_width_320", DISP_WIDTH == 320);
    ok("fb_height_200", DISP_HEIGHT == 200);

    /* Write a 10-pixel yellow horizontal line at row 50, starting col 100
     * Yellow = R=0xFF, G=0xFF, B=0x00, A=0xFF */
    for (int x = 100; x < 110; x++) {
        unsigned int off = (50 * 320 + x) * 4;
        FB_BYTES[off + 0] = 0xFF;  /* R */
        FB_BYTES[off + 1] = 0xFF;  /* G */
        FB_BYTES[off + 2] = 0x00;  /* B */
        FB_BYTES[off + 3] = 0xFF;  /* A */
    }
    ok("fb_yellow_written", 1);

    /* Write a single magenta pixel at (0,0) using word write
     * RGBA in memory: R=0xFF, G=0x00, B=0xFF, A=0xFF
     * As little-endian u32: 0xFFFF00FF */
    FB_WORDS[0] = 0xFFFF00FF;
    ok("fb_magenta_word", 1);

    /* Read back the pixel we wrote using byte access */
    ok("fb_readback_r", FB_BYTES[0] == 0xFF);
    ok("fb_readback_g", FB_BYTES[1] == 0x00);
    ok("fb_readback_b", FB_BYTES[2] == 0xFF);
    ok("fb_readback_a", FB_BYTES[3] == 0xFF);

    /* Write pixel at last position (319,199) — edge case */
    unsigned int last = (199 * 320 + 319) * 4;
    FB_BYTES[last + 0] = 0x80;
    FB_BYTES[last + 1] = 0x80;
    FB_BYTES[last + 2] = 0x80;
    FB_BYTES[last + 3] = 0xFF;
    ok("fb_last_pixel", 1);
}

/* ── 6. Display Control — palette & mode ──────────── */
static void test_display_control(void)
{
    put("[display_ctrl]\n");

    /* Set palette entry 0 = bright red (0x00FF0000) */
    DISP_PAL_IDX = 0;
    DISP_PAL_DATA = 0x00FF0000;

    /* Set palette entry 42 = dark blue (0x000000AA) */
    DISP_PAL_IDX = 42;
    DISP_PAL_DATA = 0x000000AA;

    /* Set palette entry 255 = white (0x00FFFFFF) */
    DISP_PAL_IDX = 255;
    DISP_PAL_DATA = 0x00FFFFFF;
    ok("pal_entries_set", 1);

    /* Switch to indexed mode */
    DISP_MODE = 1;
    unsigned int mode = DISP_MODE;
    ok("mode_indexed", mode == 1);

    /* Switch back to direct RGBA mode */
    DISP_MODE = 0;
    ok("mode_direct", DISP_MODE == 0);

    /* Signal vsync */
    DISP_VSYNC = 1;
    ok("vsync_set", DISP_VSYNC == 1);
}

/* ── 7. Audio — write buffer & configure ──────────── */
static void test_audio(void)
{
    put("[audio]\n");

    /* Write a simple square wave pattern: 512 bytes of 0xAA, 512 bytes of 0x55 */
    for (int i = 0; i < 512; i++) AUDIO_BUF[i] = 0xAA;
    for (int i = 512; i < 1024; i++) AUDIO_BUF[i] = 0x55;
    ok("audio_buf_written", 1);

    /* Readback */
    ok("audio_buf_read_0", AUDIO_BUF[0] == 0xAA);
    ok("audio_buf_read_511", AUDIO_BUF[511] == 0xAA);
    ok("audio_buf_read_512", AUDIO_BUF[512] == 0x55);
    ok("audio_buf_read_1023", AUDIO_BUF[1023] == 0x55);

    /* Configure: 11025 Hz, stereo, 16-bit */
    AUDIO_SRATE = 11025;
    AUDIO_CHAN  = 2;
    AUDIO_BITS  = 16;
    AUDIO_BSTART = 0;
    AUDIO_BLEN   = 1024;

    ok("audio_srate", AUDIO_SRATE == 11025);
    ok("audio_chan", AUDIO_CHAN == 2);
    ok("audio_bits", AUDIO_BITS == 16);
    ok("audio_bstart", AUDIO_BSTART == 0);
    ok("audio_blen", AUDIO_BLEN == 1024);

    /* Start playback */
    AUDIO_CTRL = 1;
    ok("audio_playing", AUDIO_STATUS != 0);

    /* Stop */
    AUDIO_CTRL = 4;
    ok("audio_stopped", (AUDIO_CTRL & 1) == 0);
}

/* ── 8. UART RX (host injects input) ─────────────── */
static void test_uart_rx(void)
{
    put("[uart_rx]\n");

    /* Host injects bytes 'H', 'i', '!' into UART RX FIFO before run */
    unsigned char lsr = UART_LSR;
    ok("uart_rx_ready", (lsr & 0x01) != 0);

    unsigned char c1 = UART_RBR;
    ok("uart_rx_H", c1 == 'H');

    unsigned char c2 = UART_RBR;
    ok("uart_rx_i", c2 == 'i');

    unsigned char c3 = UART_RBR;
    ok("uart_rx_bang", c3 == '!');

    /* FIFO empty now */
    lsr = UART_LSR;
    ok("uart_rx_empty", (lsr & 0x01) == 0);
}

/* ═════════════════════════════════════════════════════ */
void _start(void)
{
    put("mmio_devices_test\n");

    test_uart();
    test_keyboard();
    test_mouse();
    test_rtc();
    test_framebuffer();
    test_display_control();
    test_audio();
    test_uart_rx();

    put("mmio_devices_test: done\n");
    exit_emu(0);
}
