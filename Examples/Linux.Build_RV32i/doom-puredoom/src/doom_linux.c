/*
 * doom_linux.c — PureDOOM glue for the rvemu nommu RV32 Linux guest.
 *
 * Mirror of Examples/Doom/Programs/doom_main.c, adapted from bare-metal
 * to user-space Linux:
 *
 *   - main(argc,argv) instead of _start, real libc/stdio
 *   - All MMIO peripherals accessed via mmap("/dev/mem", ...) at the
 *     SAME guest-physical addresses as the bare-metal port. The Linux
 *     kernel needs CONFIG_DEVMEM=y and CONFIG_STRICT_DEVMEM=n (both
 *     enforced by Examples.Linux.Build_RV32i).
 *   - 320x200 RGBA → 2x-upscale (640x400) centered in 1024x768 FB at
 *     0x85C00000. The bare-metal port instead set DISP_FB_ADDR to
 *     point at Doom's internal buffer (zero-copy), but Examples.Linux
 *     has no DisplayControlDevice — FB is a fixed RAM slot.
 *   - WAD loaded from /usr/share/games/doom/doom1.wad via stdio
 *     (shipped by the buildroot doom-wad package).
 *   - Audio + MIDI write directly to the same MMIO addresses
 *     (0x30000000 / 0x10005004) so they bypass rvemu-audiod and
 *     rvemu-midid entirely. Faster, and side-steps any snd-aloop or
 *     snd-virmidi quirks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

/* ── MMIO base addresses (matches Examples/Doom layout) ───────────── */

#define MMIO_KBD          0x10001000UL
#define MMIO_MOUSE        0x10002000UL
#define MMIO_MIDI         0x10005000UL
#define MMIO_AUDIO_BUF    0x30000000UL
#define MMIO_AUDIO_CTL    0x30100000UL
#define MMIO_FB           0x85C00000UL

#define KBD_REGION_SIZE      0x1000UL
#define MOUSE_REGION_SIZE    0x1000UL
#define MIDI_REGION_SIZE     0x1000UL
#define AUDIO_BUF_SIZE       0x100000UL    /* 1 MB ring */
#define AUDIO_CTL_SIZE       0x1000UL
#define FB_REGION_SIZE       0x400000UL    /* 4 MB = 1024*768*4 + slack */

#define FB_WIDTH   1024
#define FB_HEIGHT  768
#define DOOM_W     320
#define DOOM_H     200
#define DOOM_SCALE 2
#define DOOM_X     ((FB_WIDTH  - DOOM_W * DOOM_SCALE) / 2)   /* = 192 */
#define DOOM_Y     ((FB_HEIGHT - DOOM_H * DOOM_SCALE) / 2)   /* = 184 */

/* Keyboard register offsets (word-indexed) */
#define KBD_STATUS_W   0
#define KBD_DATA_W     1

/* Mouse register offsets (word-indexed) */
#define MOUSE_STATUS_W 0
#define MOUSE_DX_W     1
#define MOUSE_DY_W     2
#define MOUSE_BTN_W    3

/* MIDI register offsets (word-indexed) */
#define MIDI_SHORT_W   1            /* +0x04 — packed short message      */
#define MIDI_RESET_W   2            /* +0x08 — write 1 → midiOutReset    */

/* Audio control register offsets (word-indexed) */
#define AUDIO_CTRL_W   0            /* +0x00 — play bit                 */
#define AUDIO_SRATE_W  2            /* +0x08                            */
#define AUDIO_CHAN_W   3            /* +0x0C                            */
#define AUDIO_BITS_W   4            /* +0x10                            */
#define AUDIO_BSTART_W 5            /* +0x14                            */
#define AUDIO_BLEN_W   6            /* +0x18                            */

static volatile uint32_t *g_kbd;
static volatile uint32_t *g_mouse;
static volatile uint32_t *g_midi;
static volatile uint8_t  *g_audio_buf;
static volatile uint32_t *g_audio_ctl;
static volatile uint32_t *g_fb;

static void *map_mmio(int memfd, off_t phys, size_t size, const char *name)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, phys);
    if (p == MAP_FAILED) {
        fprintf(stderr, "doom: mmap %s @ 0x%lx: %s\n",
                name, (unsigned long)phys, strerror(errno));
        exit(1);
    }
    return p;
}

/* ── PureDOOM file-I/O callbacks (wrap stdio) ──────────────────────── */

static void *fio_open(const char *fn, const char *m) { return (void *)fopen(fn, m); }
static void  fio_close(void *h)                       { if (h) fclose((FILE *)h); }
static int   fio_read(void *h, void *b, int n)        { return (int)fread(b, 1, (size_t)n, (FILE *)h); }
static int   fio_write(void *h, const void *b, int n) { return (int)fwrite(b, 1, (size_t)n, (FILE *)h); }
static int   fio_seek(void *h, int off, doom_seek_t origin)
{
    int w = (origin == DOOM_SEEK_SET) ? SEEK_SET
          : (origin == DOOM_SEEK_CUR) ? SEEK_CUR
                                      : SEEK_END;
    return fseek((FILE *)h, off, w);
}
static int   fio_tell(void *h) { return (int)ftell((FILE *)h); }
static int   fio_eof(void *h)  { return feof((FILE *)h); }

/* ── PureDOOM allocator/print/exit callbacks ──────────────────────── */

static void  pd_print(const char *s) { fputs(s, stdout); fflush(stdout); }
static void *pd_malloc(int n)        { return malloc((size_t)n); }
static void  pd_free(void *p)        { free(p); }
static void  pd_exit(int code)       { exit(code); }
/* WAD search directory. PureDOOM concatenates "$DOOMWADDIR/doom1.wad" (etc)
 * and tries to open the result via our fio_open. It NEVER honors -iwad on
 * the command line (see PureDOOM.h:9603 IdentifyVersion — argv is only
 * checked for -shdev/-regdev/-comdev test flags, never -iwad). So we
 * expose the directory through the getenv callback below.
 *
 * Set in main() once we know which directory the WAD actually lives in
 * (default /usr/share/games/doom — installed by the buildroot doom-wad
 * package — but may be overridden via -iwad <path/to/somewad.wad>). */
static const char *g_wad_dir = "/usr/share/games/doom";

static char *pd_getenv(const char *v)
{
    if (!strcmp(v, "DOOMWADDIR")) return (char *)g_wad_dir;
    if (!strcmp(v, "HOME"))       return "/root";
    if (!strcmp(v, "USER"))       return "root";
    return NULL;
}
static void pd_gettime(int *sec, int *usec)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *sec  = (int)ts.tv_sec;
    *usec = (int)(ts.tv_nsec / 1000);
}

/* ── Scancode → DOOM key ──────────────────────────────────────────────
 *
 * NOTE: this differs from Examples/Doom/Programs/doom_main.c. The
 * Examples.Linux SDL viewer (LinuxSdlViewer.cs:MapSymToLinuxKey) pushes
 * Linux input-event KEY_* codes into the keyboard MMIO FIFO so guest
 * userspace evdev sees correct scancodes. The bare-metal Examples.Doom
 * uses a different SDL viewer in Frontend/ that pushes Windows VK_*
 * codes. Same MMIO peripheral, different code system depending on which
 * frontend owns the window. Codes below match include/linux/input-event-codes.h. */

static doom_key_t scan_to_doom(unsigned scan)
{
    switch (scan) {
        /* Specials */
        case   1: return DOOM_KEY_ESCAPE;
        case  14: return DOOM_KEY_BACKSPACE;
        case  15: return DOOM_KEY_TAB;
        case  28: return DOOM_KEY_ENTER;
        case  57: return DOOM_KEY_SPACE;
        /* Arrows + nav */
        case 103: return DOOM_KEY_UP_ARROW;
        case 105: return DOOM_KEY_LEFT_ARROW;
        case 106: return DOOM_KEY_RIGHT_ARROW;
        case 108: return DOOM_KEY_DOWN_ARROW;
        /* Modifiers (Doom collapses L/R into the same key) */
        case  29: case  97: return DOOM_KEY_CTRL;
        case  42: case  54: return DOOM_KEY_SHIFT;
        case  56: case 100: return DOOM_KEY_ALT;
        /* Letters — Doom uses ASCII lowercase for these. Critical that
         * Y / N are mapped, otherwise the quit screen (which asks "are
         * you sure (y/n)?") ignores both responses and the game loops. */
        case  16: return DOOM_KEY_Q;  case  17: return DOOM_KEY_W;
        case  18: return DOOM_KEY_E;  case  19: return DOOM_KEY_R;
        case  20: return DOOM_KEY_T;  case  21: return DOOM_KEY_Y;
        case  22: return DOOM_KEY_U;  case  23: return DOOM_KEY_I;
        case  24: return DOOM_KEY_O;  case  25: return DOOM_KEY_P;
        case  30: return DOOM_KEY_A;  case  31: return DOOM_KEY_S;
        case  32: return DOOM_KEY_D;  case  33: return DOOM_KEY_F;
        case  34: return DOOM_KEY_G;  case  35: return DOOM_KEY_H;
        case  36: return DOOM_KEY_J;  case  37: return DOOM_KEY_K;
        case  38: return DOOM_KEY_L;
        case  44: return DOOM_KEY_Z;  case  45: return DOOM_KEY_X;
        case  46: return DOOM_KEY_C;  case  47: return DOOM_KEY_V;
        case  48: return DOOM_KEY_B;  case  49: return DOOM_KEY_N;
        case  50: return DOOM_KEY_M;
        /* Digits — KEY_1..KEY_9 then KEY_0 */
        case   2: return DOOM_KEY_1;  case   3: return DOOM_KEY_2;
        case   4: return DOOM_KEY_3;  case   5: return DOOM_KEY_4;
        case   6: return DOOM_KEY_5;  case   7: return DOOM_KEY_6;
        case   8: return DOOM_KEY_7;  case   9: return DOOM_KEY_8;
        case  10: return DOOM_KEY_9;  case  11: return DOOM_KEY_0;
        /* Punctuation Doom can use as binds */
        case  12: return DOOM_KEY_MINUS;
        case  13: return DOOM_KEY_EQUALS;
        case  51: return DOOM_KEY_COMMA;
        case  52: return DOOM_KEY_PERIOD;
        case  53: return DOOM_KEY_SLASH;
        case  39: return DOOM_KEY_SEMICOLON;
        case  40: return DOOM_KEY_APOSTROPHE;
        case  26: return DOOM_KEY_LEFT_BRACKET;
        case  27: return DOOM_KEY_RIGHT_BRACKET;
        /* F-keys */
        case  59: return DOOM_KEY_F1;   case  60: return DOOM_KEY_F2;
        case  61: return DOOM_KEY_F3;   case  62: return DOOM_KEY_F4;
        case  63: return DOOM_KEY_F5;   case  64: return DOOM_KEY_F6;
        case  65: return DOOM_KEY_F7;   case  66: return DOOM_KEY_F8;
        case  67: return DOOM_KEY_F9;   case  68: return DOOM_KEY_F10;
        case  87: return DOOM_KEY_F11;  case  88: return DOOM_KEY_F12;
        default:  return DOOM_KEY_UNKNOWN;
    }
}

/* ── Input polling ────────────────────────────────────────────────── */

static void poll_kbd(void)
{
    while (g_kbd[KBD_STATUS_W] & 1) {
        uint32_t raw = g_kbd[KBD_DATA_W];
        unsigned scan = raw & 0xFF;
        int pressed   = (raw & 0x100) != 0;
        doom_key_t k  = scan_to_doom(scan);
        if (pressed) doom_key_down(k); else doom_key_up(k);
    }
}

static void poll_mouse(void)
{
    if (!(g_mouse[MOUSE_STATUS_W] & 1)) return;

    int32_t  dx  = (int32_t)g_mouse[MOUSE_DX_W] * 4;
    int32_t  dy  = (int32_t)g_mouse[MOUSE_DY_W] * 4;
    uint32_t btn = g_mouse[MOUSE_BTN_W];

    if (dx || dy) doom_mouse_move(dx, dy);

    static uint32_t prev_btn = 0;
    uint32_t chg = btn ^ prev_btn;
    if (chg & 1) { if (btn & 1) doom_button_down(DOOM_LEFT_BUTTON);  else doom_button_up(DOOM_LEFT_BUTTON);  }
    if (chg & 2) { if (btn & 2) doom_button_down(DOOM_RIGHT_BUTTON); else doom_button_up(DOOM_RIGHT_BUTTON); }
    if (chg & 4) { if (btn & 4) doom_button_down(DOOM_MIDDLE_BUTTON); else doom_button_up(DOOM_MIDDLE_BUTTON); }
    prev_btn = btn;
}

/* ── Framebuffer blit: 320x200 → 640x400 centered in 1024x768 ─────── */

static void blit_fb(void)
{
    const uint32_t *src = (const uint32_t *)doom_get_framebuffer(4);
    if (!src) return;

    for (int y = 0; y < DOOM_H; y++) {
        uint32_t *dst0 = (uint32_t *)g_fb + (DOOM_Y + y * 2 + 0) * FB_WIDTH + DOOM_X;
        uint32_t *dst1 = (uint32_t *)g_fb + (DOOM_Y + y * 2 + 1) * FB_WIDTH + DOOM_X;
        const uint32_t *row = src + y * DOOM_W;
        for (int x = 0; x < DOOM_W; x++) {
            uint32_t px = row[x];
            dst0[x * 2 + 0] = px;
            dst0[x * 2 + 1] = px;
            dst1[x * 2 + 0] = px;
            dst1[x * 2 + 1] = px;
        }
    }
}

/* ── Audio copy: identical handshake to Examples/Doom ─────────────── */

static short clamp16(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (short)v;
}

static void copy_audio(void)
{
    short *buf = doom_get_sound_buffer();
    if (!buf) return;

    /* Wait for host to consume previous buffer. Host clears Ctrl bit 0
     * after queueing to SDL. Don't spin forever — if --gui wasn't used
     * the host audio thread isn't running and Ctrl never clears. */
    int spins = 0;
    while (g_audio_ctl[AUDIO_CTRL_W] & 1) {
        if (++spins > 10000) { g_audio_ctl[AUDIO_CTRL_W] = 0; break; }
    }

    volatile short *dst = (volatile short *)g_audio_buf;
    int nsamples = 512 * 2;
    for (int i = 0; i < nsamples; i++)
        dst[i] = clamp16((int)buf[i] * 4);

    g_audio_ctl[AUDIO_SRATE_W]  = 11025;
    g_audio_ctl[AUDIO_CHAN_W]   = 2;
    g_audio_ctl[AUDIO_BITS_W]   = 16;
    g_audio_ctl[AUDIO_BSTART_W] = 0;
    g_audio_ctl[AUDIO_BLEN_W]   = 2048;
    g_audio_ctl[AUDIO_CTRL_W]   = 1;    /* play */
}

/* ── Exit cleanup: silence the Windows MIDI synth on quit ────────────
 *
 * Without this, whatever note was playing when the user picked "Quit"
 * keeps sustaining in the host synth — Doom stopped feeding new MIDI
 * messages but never sent Note Off. MidiDevice +0x08 is wired to
 * midiOutReset() on the host side, which stops all notes on all
 * channels. Registered via atexit so it fires on the normal pd_exit()
 * path (Y on the quit screen) and on main() returning. */
static void midi_silence_on_exit(void)
{
    if (g_midi) g_midi[MIDI_RESET_W] = 1;
}

/* ── Time helpers ─────────────────────────────────────────────────── */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    /* WAD location. Default is the buildroot doom-wad install path.
     * `-iwad <path>` overrides it; we split into dir + filename so
     * IdentifyVersion's `$DOOMWADDIR/doom1.wad` join finds it. */
    const char *wad_path = "/usr/share/games/doom/doom1.wad";
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-iwad")) wad_path = argv[i + 1];

    /* Sanity-check WAD before doing anything else. fopen via callback
     * later would return NULL and Doom emits a confusing
     * "W_InitFiles: no files found" instead of a path error. */
    {
        FILE *f = fopen(wad_path, "rb");
        if (!f) {
            fprintf(stderr, "doom: cannot open WAD %s: %s\n",
                    wad_path, strerror(errno));
            return 1;
        }
        fclose(f);
    }

    /* Derive DOOMWADDIR from wad_path (strip trailing /filename). PureDOOM
     * concatenates "$DOOMWADDIR" + "/" + "doom1.wad", so we want the dir
     * without the trailing slash. */
    {
        static char dir_buf[512];
        const char *slash = strrchr(wad_path, '/');
        if (slash && slash != wad_path) {
            size_t n = (size_t)(slash - wad_path);
            if (n >= sizeof(dir_buf)) n = sizeof(dir_buf) - 1;
            memcpy(dir_buf, wad_path, n);
            dir_buf[n] = '\0';
            g_wad_dir = dir_buf;
        }
    }

    /* Stop rvemu-input before claiming the keyboard FIFO. Both daemons
     * mmap the same /dev/mem region at 0x10001000 and call the same
     * 'read pops a scancode' MMIO protocol — if both poll, each event
     * goes to exactly one reader and Doom sees ~half the keys. The
     * wrapper at /usr/bin/doom also stops it via S42input, but doing it
     * here too means a direct /usr/libexec/doom run works without
     * keyboard glitching. The wrapper handles the restart on exit. */
    {
        FILE *pf = fopen("/var/run/rvemu-input.pid", "r");
        if (pf) {
            int pid = 0;
            if (fscanf(pf, "%d", &pid) == 1 && pid > 1)
                kill(pid, SIGTERM);
            fclose(pf);
        }
    }

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("doom: open /dev/mem"); return 1; }

    g_kbd       = map_mmio(mem, MMIO_KBD,       KBD_REGION_SIZE,   "kbd");
    g_mouse     = map_mmio(mem, MMIO_MOUSE,     MOUSE_REGION_SIZE, "mouse");
    g_midi      = map_mmio(mem, MMIO_MIDI,      MIDI_REGION_SIZE,  "midi");
    g_audio_buf = map_mmio(mem, MMIO_AUDIO_BUF, AUDIO_BUF_SIZE,    "audio buf");
    g_audio_ctl = map_mmio(mem, MMIO_AUDIO_CTL, AUDIO_CTL_SIZE,    "audio ctl");
    g_fb        = map_mmio(mem, MMIO_FB,        FB_REGION_SIZE,    "framebuffer");

    atexit(midi_silence_on_exit);

    /* Clear the FB so the area outside Doom's centered viewport is black
     * (otherwise we get garbage from whatever nano-X left behind). */
    for (unsigned i = 0; i < FB_WIDTH * FB_HEIGHT; i++) g_fb[i] = 0xFF000000u;

    /* PureDOOM wiring */
    doom_set_print(pd_print);
    doom_set_malloc(pd_malloc, pd_free);
    doom_set_file_io(fio_open, fio_close, fio_read, fio_write,
                     fio_seek, fio_tell, fio_eof);
    doom_set_gettime(pd_gettime);
    doom_set_exit(pd_exit);
    doom_set_getenv(pd_getenv);

    doom_set_default_int("key_up",            DOOM_KEY_W);
    doom_set_default_int("key_down",          DOOM_KEY_S);
    doom_set_default_int("key_strafeleft",    DOOM_KEY_A);
    doom_set_default_int("key_straferight",   DOOM_KEY_D);
    doom_set_default_int("key_use",           DOOM_KEY_E);
    doom_set_default_int("mouse_move",        0);
    doom_set_default_int("mouse_sensitivity", 9);

    /* Cap PureDOOM's zone allocator at 3 MB. The default 6 MB routinely
     * fails on our nommu uclibc: kernel /proc/buddyinfo shows fragmentation
     * across the 96 MB bank after Microwindows brings up — largest
     * contiguous free chunk is ~4 MB, and nommu uclibc malloc cannot
     * satisfy a 6 MB single-allocation request.
     *
     * PureDOOM stores the zone size in the global `mb_used` and DOES NOT
     * honor `-mb` from argv (see PureDOOM.h:16248 — the global is only
     * initialised, never reassigned). Override it directly before
     * doom_init() calls Z_Init(). */
    extern int mb_used;
    mb_used = 3;

    char *doom_argv[] = { "doom", "-iwad", (char *)wad_path };
    doom_init(3, doom_argv, 0);

    /* Force WASD again post-init in case the cached cfg overrode us. */
    {
        extern int key_up, key_down, key_strafeleft, key_straferight, key_use;
        key_up          = DOOM_KEY_W;
        key_down        = DOOM_KEY_S;
        key_strafeleft  = DOOM_KEY_A;
        key_straferight = DOOM_KEY_D;
        key_use         = DOOM_KEY_E;
    }

    pd_print("doom_linux: entering game loop\n");

    uint64_t last_audio_us = 0;
    uint64_t last_midi_us  = now_us();
    uint64_t midi_accum_us = 0;
    const uint64_t MIDI_TICK_US = 7143;   /* 1_000_000 / 140 */

    while (1) {
        poll_kbd();
        poll_mouse();
        doom_update();
        blit_fb();

        uint64_t t = now_us();
        if (t - last_audio_us >= 46440) {     /* 512 / 11025 s */
            last_audio_us = t;
            copy_audio();
        }

        uint64_t midi_delta = t - last_midi_us;
        last_midi_us = t;
        midi_accum_us += midi_delta;
        while (midi_accum_us >= MIDI_TICK_US) {
            midi_accum_us -= MIDI_TICK_US;
            unsigned long m;
            while ((m = doom_tick_midi()) != 0)
                g_midi[MIDI_SHORT_W] = (uint32_t)m;
        }
    }
    return 0;
}
