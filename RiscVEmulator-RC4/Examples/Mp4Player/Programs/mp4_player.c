/* mp4_player.c — Video player for the RV32I emulator.
 *
 * Plays video+audio from a delta-RLE pre-encoded format (SVID).
 * The host creates the SVID from an MP4 via ffmpeg and serves it
 * through a DiskDevice peripheral. The guest streams frame data
 * on demand — no need to fit the whole file in RAM.
 */

#include "libc.h"

/* ── Hardware addresses ────────────────────────────────────────────── */
#define FB_BASE      ((volatile unsigned char *)0x20000000)
#define DISP_WIDTH   (*(volatile unsigned int *)0x20100000)
#define DISP_HEIGHT  (*(volatile unsigned int *)0x20100004)
#define DISP_VSYNC   (*(volatile unsigned int *)0x2010000C)
#define DISP_FB_ADDR (*(volatile unsigned int *)0x2010001C)

#define AUDIO_BUF    ((volatile unsigned char *)0x30000000)
#define AUDIO_CTRL   (*(volatile unsigned int *)0x30100000)
#define AUDIO_SRATE  (*(volatile unsigned int *)0x30100008)
#define AUDIO_CHAN   (*(volatile unsigned int *)0x3010000C)
#define AUDIO_BITS   (*(volatile unsigned int *)0x30100010)
#define AUDIO_BSTART (*(volatile unsigned int *)0x30100014)
#define AUDIO_BLEN   (*(volatile unsigned int *)0x30100018)

#define KB_STATUS    (*(volatile unsigned int *)0x10001000)
#define KB_DATA      (*(volatile unsigned int *)0x10001004)

#define RTC_MS_LO    (*(volatile unsigned int *)0x10003008)

#define KEY_LEFT  0x25
#define KEY_RIGHT 0x27
#define KEY_SPACE 0x20

/* ── Disk device registers ────────────────────────────────────────── */
#define DISK_OFFSET_LO (*(volatile unsigned int *)0x10004000)
#define DISK_OFFSET_HI (*(volatile unsigned int *)0x10004004)
#define DISK_LENGTH    (*(volatile unsigned int *)0x10004008)
#define DISK_DEST_ADDR (*(volatile unsigned int *)0x1000400C)
#define DISK_CMD       (*(volatile unsigned int *)0x10004010)
#define DISK_STATUS    (*(volatile unsigned int *)0x10004014)
#define DISK_FSIZE_LO  (*(volatile unsigned int *)0x10004018)
#define DISK_FSIZE_HI  (*(volatile unsigned int *)0x1000401C)

static void disk_read(unsigned int file_offset, unsigned int length, void *dest)
{
    DISK_OFFSET_LO = file_offset;
    DISK_OFFSET_HI = 0;
    DISK_LENGTH    = length;
    DISK_DEST_ADDR = (unsigned int)dest;
    DISK_CMD       = 1;
    /* Host completes synchronously. Compiler barrier ensures subsequent
     * reads from *dest see the data written by the host via bus.Load(). */
    __asm__ volatile("" ::: "memory");
}

/* ── SVID format ──────────────────────────────────────────────────── */
#define SVID_MAGIC  0x44495653  /* "SVID" */

typedef struct {
    unsigned int   magic;
    unsigned short width, height;
    unsigned short fps;
    unsigned short audio_rate;
    unsigned short audio_channels;
    unsigned short num_frames;
    unsigned int   reserved1, reserved2;
} svid_header_t;   /* 24 bytes */

typedef struct {
    unsigned int offset;
    unsigned int video_size;
    unsigned int audio_size;
} frame_entry_t;   /* 12 bytes */

/* Buffers in guest RAM — placed at a high address to avoid code/stack */
#define FRAME_BUF_ADDR  0x00A00000
#define IO_BUF_ADDR     0x00C00000  /* scratch for disk reads */

#define MAX_PIXELS (320 * 200)

static unsigned int *frame_buf = (unsigned int *)FRAME_BUF_ADDR;

/* Audio double-buffer */
#define AUDIO_BUF_BYTES 8192
static int audio_active_buf;

/* ── Delta-RLE video decode ──────────────────────────────────────── */
static void decode_video(unsigned char *data, unsigned int size_bytes)
{
    memcpy(frame_buf, data, size_bytes);
}

/* ── Audio output ─────────────────────────────────────────────────── */
static void play_audio(unsigned int *data, unsigned int size)
{
    if (size == 0) return;

    while (AUDIO_CTRL & 1)
        ;

    unsigned int off = audio_active_buf ? AUDIO_BUF_BYTES : 0;
    unsigned int n   = size < AUDIO_BUF_BYTES ? size : AUDIO_BUF_BYTES;

    memcpy((void *)(AUDIO_BUF + off), data, n);

    AUDIO_BSTART = off;
    AUDIO_BLEN   = n;
    AUDIO_CTRL   = 1;

    audio_active_buf ^= 1;
}

/* ── Entry point ──────────────────────────────────────────────────── */
void _start(void)
{
    printf("SVID Player: starting\n");

    unsigned int file_size = DISK_FSIZE_LO;
    printf("  Disk file: %u bytes\n", file_size);

    if (file_size < sizeof(svid_header_t)) {
        printf("ERROR: no data on disk\n");
        exit(1);
    }

    /* Read header from disk into a raw buffer and parse manually
     * to avoid any struct packing ambiguity */
    unsigned char hdr_raw[24];
    disk_read(0, 24, hdr_raw);

    unsigned int magic = *(unsigned int *)(hdr_raw + 0);
    if (magic != SVID_MAGIC) {
        printf("ERROR: bad SVID magic 0x%x\n", magic);
        exit(1);
    }

    unsigned int w   = *(unsigned short *)(hdr_raw + 4);
    unsigned int h   = *(unsigned short *)(hdr_raw + 6);
    unsigned int fps = *(unsigned short *)(hdr_raw + 8);
    unsigned int audio_rate = *(unsigned short *)(hdr_raw + 10);
    unsigned int audio_channels = *(unsigned short *)(hdr_raw + 12);
    unsigned int nf  = *(unsigned short *)(hdr_raw + 14);
    unsigned int pixels = w * h;

    printf("  Video: %ux%u @ %u fps, %u frames\n", w, h, fps, nf);
    printf("  Audio: %u Hz, %u ch\n",
           audio_rate, audio_channels);

    DISP_WIDTH  = w;
    DISP_HEIGHT = h;
    DISP_FB_ADDR = (unsigned int)frame_buf;  /* host reads from RAM on vsync */

    if (audio_rate) {
        AUDIO_SRATE = audio_rate;
        AUDIO_CHAN  = audio_channels;
        AUDIO_BITS  = 16;
    }

    /* Read frame table from disk */
    unsigned int ftab_size = nf * sizeof(frame_entry_t);
    frame_entry_t *ftab = (frame_entry_t *)IO_BUF_ADDR;
    disk_read(sizeof(svid_header_t), ftab_size, ftab);

    /* Data starts after header + frame table */
    unsigned int data_base = sizeof(svid_header_t) + ftab_size;

    /* IO buffer for frame data — placed after frame table in RAM */
    unsigned char *io_buf = (unsigned char *)IO_BUF_ADDR + ftab_size;
    /* Align to 4 bytes */
    io_buf = (unsigned char *)(((unsigned int)io_buf + 3) & ~3u);

    memset(frame_buf, 0, pixels * 4);
    audio_active_buf = 0;

    unsigned int ms_per_frame = 1000 / fps;
    unsigned int next_ms      = RTC_MS_LO;
    unsigned int fps_timer    = next_ms;
    unsigned int shown        = 0;
    unsigned int fi           = 0;
    int paused                = 0;
    unsigned int seek_frames  = fps * 5;  /* 5 seconds per seek step */

    printf("  Playing (loop)...\n");
    printf("  Controls: LEFT/RIGHT = seek 5s, SPACE = pause\n");

    while (1) {
        /* Poll keyboard for seek/pause controls */
        while (KB_STATUS & 1) {
            unsigned int key = KB_DATA;
            unsigned int code = key & 0xFF;
            int pressed = key & 0x100;
            if (pressed) {
                if (code == KEY_RIGHT) {
                    fi += seek_frames;
                    if (fi >= nf) fi = nf - 1;
                    next_ms = RTC_MS_LO;
                    printf(">> seek to frame %u/%u\n", fi, nf);
                } else if (code == KEY_LEFT) {
                    if (fi >= seek_frames) fi -= seek_frames;
                    else fi = 0;
                    next_ms = RTC_MS_LO;
                    printf("<< seek to frame %u/%u\n", fi, nf);
                } else if (code == KEY_SPACE) {
                    paused = !paused;
                    if (paused) {
                        /* Wait for host to consume, then stop audio */
                        while (AUDIO_CTRL & 1)
                            ;
                        AUDIO_CTRL = 4;  /* stop: clears ctrl + position */
                    } else {
                        audio_active_buf = 0;
                        next_ms = RTC_MS_LO;
                    }
                    printf(paused ? "|| paused\n" : "|> resumed\n");
                }
            }
        }

        if (paused) {
            for (volatile int delay = 0; delay < 10000; delay++) {}
            continue;
        }

        /* Reduce MMIO overhead: only poll RTC every ~10000 native iterations */
        unsigned int now;
        do {
            for (volatile int delay = 0; delay < 5000; delay++) {}
            now = RTC_MS_LO;
        } while ((int)(now - next_ms) < 0);

        next_ms += ms_per_frame;
        if ((int)(now - next_ms) > (int)(ms_per_frame * 3))
            next_ms = now;

        frame_entry_t *fe = &ftab[fi];
        unsigned int frame_total = fe->video_size + fe->audio_size;

        /* Read this frame's data from disk into io_buf */
        disk_read(data_base + fe->offset, frame_total, io_buf);

        decode_video(io_buf, fe->video_size);
        DISP_VSYNC = 1;  /* host blits from frame_buf in RAM */

        play_audio((unsigned int *)(io_buf + fe->video_size), fe->audio_size);

        shown++;
        fi++;
        if (fi >= nf) {
            fi = 0;
        }

        if (now - fps_timer >= 1000) {
            printf("frame %u\n", shown);
            fps_timer = now;
        }
    }
}
