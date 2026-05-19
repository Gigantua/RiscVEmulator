/* doom_main.c — PureDoom glue for the RV32I emulator.
 *
 * Wires PureDOOM's callback hooks to our MMIO peripherals:
 *   - UART for print output
 *   - Timer for gettime
 *   - Keyboard/Mouse for input
 *   - Framebuffer for video
 *   - Audio buffer for sound
 *   - VFS for WAD file loading
 *
 * Compile:
 *   clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
 *         -nostdlib -nostartfiles -O3 -fno-builtin -fuse-ld=lld \
 *         -Wl,-Tlinker.ld doom_main.c malloc.o vfs.o runtime.o -o doom.elf
 *
 * The WAD file must be loaded into RAM by the host at WAD_BASE_ADDR
 * with its size written to WAD_SIZE_ADDR before execution begins.
 */

#include "libc.h"

#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

/* ── MMIO Peripheral Addresses ─────────────────────────────────── */

/* Keyboard (scancode FIFO) */
#define KB_STATUS   (*(volatile unsigned int *)0x10001000)
#define KB_DATA     (*(volatile unsigned int *)0x10001004)

/* Mouse (delta-on-read) */
#define MOUSE_STATUS (*(volatile unsigned int *)0x10002000)
#define MOUSE_DX     (*(volatile int *)0x10002004)
#define MOUSE_DY     (*(volatile int *)0x10002008)
#define MOUSE_BTN    (*(volatile unsigned int *)0x1000200C)

/* Display control */
#define DISP_VSYNC   (*(volatile unsigned int *)0x2010000C)
#define DISP_FB_ADDR (*(volatile unsigned int *)0x2010001C)

/* Timer (CLINT: mtime as instruction count) */
#define TIMER_LO    (*(volatile unsigned int *)0x0200BFF8)
#define TIMER_HI    (*(volatile unsigned int *)0x0200BFFC)

/* Real-Time Clock (wall-clock microseconds from host) */
#define RTC_US_LO   (*(volatile unsigned int *)0x10003000)
#define RTC_US_HI   (*(volatile unsigned int *)0x10003004)
#define RTC_MS_LO   (*(volatile unsigned int *)0x10003008)
#define RTC_EPOCH_LO (*(volatile unsigned int *)0x10003010)
#define RTC_SECS_LO  (*(volatile unsigned int *)0x10003018) /* seconds since boot */
#define RTC_USEC_FRAC (*(volatile unsigned int *)0x1000301C) /* usec within second (0–999999) */

/* Audio buffer + control */
#define AUDIO_BUF   ((volatile unsigned char *)0x30000000)
#define AUDIO_CTRL  (*(volatile unsigned int *)0x30100000)  /* +0x00 */
#define AUDIO_SRATE (*(volatile unsigned int *)0x30100008)  /* +0x08 */
#define AUDIO_CHAN  (*(volatile unsigned int *)0x3010000C)   /* +0x0C */
#define AUDIO_BITS  (*(volatile unsigned int *)0x30100010)   /* +0x10 */
#define AUDIO_BSTART (*(volatile unsigned int *)0x30100014)  /* +0x14 */
#define AUDIO_BLEN  (*(volatile unsigned int *)0x30100018)   /* +0x18 */
#define AUDIO_POS   (*(volatile unsigned int *)0x3010001C)   /* +0x1C */

/* MIDI output device */
#define MIDI_STATUS (*(volatile unsigned int *)0x10005000)
#define MIDI_DATA   (*(volatile unsigned int *)0x10005004)
#define MIDI_CTRL   (*(volatile unsigned int *)0x10005008)

/* WAD location — host writes WAD data here and sets the size */
#define WAD_BASE_ADDR  0x00A00000u  /* 10 MB offset into RAM */
#define WAD_SIZE_ADDR  (*(volatile unsigned int *)0x009FFFFCu) /* size stored just before WAD */

/* ── VFS declarations ──────────────────────────────────────────── */
void vfs_register(const char *name, void *data, unsigned int size);
void *vfs_open(const char *filename, const char *mode);
void vfs_close(void *handle);
int vfs_read(void *handle, void *buf, int count);
int vfs_write(void *handle, const void *buf, int count);
int vfs_seek(void *handle, int offset, unsigned int origin);

static int my_seek(void *handle, int offset, doom_seek_t origin)
{
    return vfs_seek(handle, offset, (unsigned int)origin);
}
int vfs_tell(void *handle);
int vfs_eof(void *handle);

/* ── Callback Implementations ──────────────────────────────────── */

static void my_print(const char *str)
{
    printf("%s", str);
}

static void *my_malloc(int size)
{
    return malloc(size);
}

static void my_free(void *ptr)
{
    free(ptr);
}

static void my_gettime(int *sec, int *usec)
{
    /* Use pre-split RTC registers — no 64-bit division needed */
    *sec  = (int)RTC_SECS_LO;
    *usec = (int)RTC_USEC_FRAC;
}

static void my_exit(int code)
{
    exit(code);
}

static char *my_getenv(const char *var)
{
    if (strcmp(var, "HOME") == 0) return "/home";
    if (strcmp(var, "USER") == 0) return "doom";
    return (char *)0;
}

/* ── Scancode to DOOM key mapping ──────────────────────────────── */
static doom_key_t scancode_to_doom_key(unsigned int scancode)
{
    /* Map common scancodes to DOOM keys.
     * Our keyboard peripheral uses a subset of USB HID scancodes. */
    switch (scancode) {
        case 0x1B: return DOOM_KEY_ESCAPE;
        case 0x0D: return DOOM_KEY_ENTER;
        case 0x09: return DOOM_KEY_TAB;
        case 0x20: return DOOM_KEY_SPACE;

        /* Arrow keys */
        case 0x25: return DOOM_KEY_LEFT_ARROW;
        case 0x26: return DOOM_KEY_UP_ARROW;
        case 0x27: return DOOM_KEY_RIGHT_ARROW;
        case 0x28: return DOOM_KEY_DOWN_ARROW;

        /* Control keys */
        case 0x10: return DOOM_KEY_SHIFT;
        case 0x11: return DOOM_KEY_CTRL;
        case 0x12: return DOOM_KEY_ALT;

        /* WASD */
        case 'W': case 'w': return DOOM_KEY_W;
        case 'A': case 'a': return DOOM_KEY_A;
        case 'S': case 's': return DOOM_KEY_S;
        case 'D': case 'd': return DOOM_KEY_D;
        case 'E': case 'e': return DOOM_KEY_E;

        /* Weapons */
        case '1': return DOOM_KEY_1;
        case '2': return DOOM_KEY_2;
        case '3': return DOOM_KEY_3;
        case '4': return DOOM_KEY_4;
        case '5': return DOOM_KEY_5;
        case '6': return DOOM_KEY_6;
        case '7': return DOOM_KEY_7;

        default: return (doom_key_t)scancode;
    }
}

/* ── Input Polling ─────────────────────────────────────────────── */
static void poll_keyboard(void)
{
    while (KB_STATUS & 1) {
        unsigned int raw = KB_DATA;
        unsigned int scancode = raw & 0xFF;
        int pressed = (raw & 0x100) != 0;
        doom_key_t key = scancode_to_doom_key(scancode);
        if (pressed)
            doom_key_down(key);
        else
            doom_key_up(key);
    }
}

static void poll_mouse(void)
{
    if (MOUSE_STATUS & 1) {
        int dx = MOUSE_DX * 4;
        int dy = MOUSE_DY * 4;
        unsigned int btn = MOUSE_BTN;

        if (dx != 0 || dy != 0)
            doom_mouse_move(dx, dy);

        /* Map mouse buttons: bit 0 = left, bit 1 = right, bit 2 = middle */
        static unsigned int prev_btn = 0;
        unsigned int changed = btn ^ prev_btn;
        if (changed & 1) {
            if (btn & 1) doom_button_down(DOOM_LEFT_BUTTON);
            else         doom_button_up(DOOM_LEFT_BUTTON);
        }
        if (changed & 2) {
            if (btn & 2) doom_button_down(DOOM_RIGHT_BUTTON);
            else         doom_button_up(DOOM_RIGHT_BUTTON);
        }
        if (changed & 4) {
            if (btn & 4) doom_button_down(DOOM_MIDDLE_BUTTON);
            else         doom_button_up(DOOM_MIDDLE_BUTTON);
        }
        prev_btn = btn;
    }
}

/* ── Framebuffer ──────────────────────────────────────────────── */
static int fb_addr_set = 0;

static void copy_framebuffer(void)
{
    const unsigned char *src = doom_get_framebuffer(4); /* RGBA */
    if (!src) return;

    /* Point display at Doom's internal RGBA buffer in RAM — zero-copy */
    if (!fb_addr_set) {
        DISP_FB_ADDR = (unsigned int)src;
        fb_addr_set = 1;
    }
    DISP_VSYNC = 1;
}

/* ── Audio Copy ────────────────────────────────────────────────── */
static short clamp16(int v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return (short)-32768;
    return (short)v;
}

static void copy_audio(void)
{
    short *buf = doom_get_sound_buffer();
    if (!buf) return;

    /* Wait for host to consume previous buffer */
    while (AUDIO_CTRL & 1)
        ;

    /* Amplify samples by 4× before copying to MMIO */
    volatile short *dst = (volatile short *)AUDIO_BUF;
    int nsamples = 512 * 2; /* 512 frames × 2 channels */
    for (int i = 0; i < nsamples; i++) {
        dst[i] = clamp16((int)buf[i] * 4);
    }

    /* Update audio control */
    AUDIO_SRATE = 11025;
    AUDIO_CHAN  = 2;
    AUDIO_BITS  = 16;
    AUDIO_BSTART = 0;      /* data starts at offset 0 in audio buffer */
    AUDIO_BLEN  = 2048;
    AUDIO_CTRL  = 1; /* play */
}

/* ── Entry Point ───────────────────────────────────────────────── */
void _start(void)
{
    my_print("doom_main: starting\n");

    /* Register WAD from host-provided memory location */
    unsigned int wad_size = WAD_SIZE_ADDR;
    if (wad_size > 0) {
        vfs_register("doom1.wad", (void *)WAD_BASE_ADDR, wad_size);
        my_print("doom_main: WAD registered\n");
    } else {
        my_print("doom_main: ERROR no WAD found\n");
        my_exit(1);
    }

    /* Set PureDoom callbacks */
    doom_set_print(my_print);
    doom_set_malloc(my_malloc, my_free);
    doom_set_file_io(
        vfs_open, vfs_close, vfs_read, vfs_write,
        my_seek, vfs_tell, vfs_eof);
    doom_set_gettime(my_gettime);
    doom_set_exit(my_exit);
    doom_set_getenv(my_getenv);

    /* Modern key bindings (WASD + mouse look) — set defaults before init */
    doom_set_default_int("key_up",          DOOM_KEY_W);
    doom_set_default_int("key_down",        DOOM_KEY_S);
    doom_set_default_int("key_strafeleft",  DOOM_KEY_A);
    doom_set_default_int("key_straferight", DOOM_KEY_D);
    doom_set_default_int("key_use",         DOOM_KEY_E);
    doom_set_default_int("mouse_move",      0);
    doom_set_default_int("mouse_sensitivity", 9);  /* max in-game (0–9) */

    /* Initialize DOOM */
    char *argv[] = { "doom", "-iwad", "doom1.wad" };
    doom_init(3, argv, 0);

    /* Force key bindings directly (bypass defaults system to be sure) */
    {
        extern int key_up, key_down, key_strafeleft, key_straferight;
        extern int key_use, key_fire;
        key_up          = DOOM_KEY_W;
        key_down        = DOOM_KEY_S;
        key_strafeleft  = DOOM_KEY_A;
        key_straferight = DOOM_KEY_D;
        key_use         = DOOM_KEY_E;
    }

    my_print("doom_main: initialized, entering game loop\n");

    /* Track time for MIDI ticking (140 ticks/sec = 7142 us per tick) */
    unsigned int midi_last_us_lo = RTC_US_LO;
    unsigned int midi_last_us_hi = RTC_US_HI;
    unsigned int midi_accum_us = 0;
    const unsigned int MIDI_TICK_US = 7143;  /* 1000000 / 140 ≈ 7143 us */

    /* Main game loop */
    while (1) {
        poll_keyboard();
        poll_mouse();
        doom_update();
        copy_framebuffer();

        /* Audio: must be called at exactly the buffer-fill rate (512 samples @ 11025 Hz).
         * doom_get_sound_buffer() internally calls I_UpdateSound() which advances every
         * active channel by SAMPLECOUNT (512) positions — calling it too fast consumes
         * sounds before SDL can queue them.  46440 µs = 512*1000000/11025. */
        {
            static unsigned int last_audio_us = 0;
            unsigned int now_audio_us = RTC_US_LO;
            if ((unsigned int)(now_audio_us - last_audio_us) >= 46440u) {
                last_audio_us = now_audio_us;
                copy_audio();
            }
        }

        /* Advance MIDI at 140 Hz using wall-clock time */
        {
            unsigned int now_lo = RTC_US_LO;
            unsigned int now_hi = RTC_US_HI;
            unsigned long long now = ((unsigned long long)now_hi << 32) | now_lo;
            unsigned long long last = ((unsigned long long)midi_last_us_hi << 32) | midi_last_us_lo;
            unsigned int delta_us = (unsigned int)(now - last);
            midi_last_us_lo = now_lo;
            midi_last_us_hi = now_hi;

            midi_accum_us += delta_us;
            while (midi_accum_us >= MIDI_TICK_US) {
                midi_accum_us -= MIDI_TICK_US;
                unsigned long midi;
                while ((midi = doom_tick_midi()) != 0) {
                    MIDI_DATA = (unsigned int)midi;
                }
            }
        }
    }
}
