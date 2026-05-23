/* sound_demo.c — Audio demo for the RV32I emulator.
 *
 * Generates a sine wave tone using double-buffered audio.
 * Reads the keyboard peripheral to allow pitch/volume adjustment:
 *   UP arrow / 'w' — increase frequency
 *   DOWN arrow / 's' — decrease frequency
 *   LEFT / 'a' — decrease volume
 *   RIGHT / 'd' — increase volume
 *
 * Compile:
 *   clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
 *         -nostdlib -nostartfiles -O3 -fno-builtin -fuse-ld=lld \
 *         -Wl,-Tlinker.ld sound_demo.c runtime.o -o sound_demo.elf
 */

#include "libc.h"

/* Keyboard */
#define KB_STATUS    (*(volatile unsigned int *)0x10001000)
#define KB_DATA      (*(volatile unsigned int *)0x10001004)

/* Audio buffer + control */
#define AUDIO_BUF    ((volatile unsigned int *)0x30000000)
#define AUDIO_CTRL   (*(volatile unsigned int *)0x30100000)
#define AUDIO_SRATE  (*(volatile unsigned int *)0x30100008)
#define AUDIO_CHAN   (*(volatile unsigned int *)0x3010000C)
#define AUDIO_BITS   (*(volatile unsigned int *)0x30100010)
#define AUDIO_BSTART (*(volatile unsigned int *)0x30100014)
#define AUDIO_BLEN   (*(volatile unsigned int *)0x30100018)

/* RTC */
#define RTC_MS_LO    (*(volatile unsigned int *)0x10003008)

#define SAMPLE_RATE  22050
#define BUF_SAMPLES  2048   /* samples per half-buffer */
#define BUF_BYTES    (BUF_SAMPLES * 2 * 2)  /* stereo 16-bit */
#define BUF1_OFFSET  0
#define BUF2_OFFSET  BUF_BYTES

/* 256-entryfull-cycle sine table: sin(i * 2π / 256) × 32767 */
static const short sine_tbl[256] = {
        0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
     6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
    12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
    18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
    23170,  23731,  24279,  24811,  25330,  25832,  26319,  26790,
    27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
    30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
    32138,  32285,  32412,  32521,  32610,  32679,  32728,  32757,
    32767,  32757,  32728,  32679,  32610,  32521,  32412,  32285,
    32138,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
    30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
    27245,  26790,  26319,  25832,  25330,  24811,  24279,  23731,
    23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
    18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
    12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
     6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
        0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
    -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
   -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
   -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
   -23170, -23731, -24279, -24811, -25330, -25832, -26319, -26790,
   -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
   -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
   -32138, -32285, -32412, -32521, -32610, -32679, -32728, -32757,
   -32767, -32757, -32728, -32679, -32610, -32521, -32412, -32285,
   -32138, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
   -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
   -27245, -26790, -26319, -25832, -25330, -24811, -24279, -23731,
   -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
   -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
   -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
    -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

/* Fill a sine wave into one half of the audio buffer.
 * buf_offset: byte offset into AUDIO_BUF (0 or BUF_BYTES)
 * freq: frequency in Hz, volume: 0–32767
 * phase_acc: 16.8 fixed-point phase accumulator
 */
static void fill_sine(unsigned int buf_offset, int freq, int volume,
                       unsigned int *phase_acc)
{
    volatile short *buf = (volatile short *)((char *)AUDIO_BUF + buf_offset);
    unsigned int step = ((unsigned int)freq << 16) / SAMPLE_RATE;
    if (step < 1) step = 1;

    unsigned int ph = *phase_acc;
    for (int i = 0; i < BUF_SAMPLES; i++)
    {
        int idx = (ph >> 8) & 0xFF;
        int raw = sine_tbl[idx];
        short val = (short)((raw * volume) / 32767);
        buf[i * 2]     = val;
        buf[i * 2 + 1] = val;
        ph += step;
    }
    *phase_acc = ph;
}

/* Submit a filled buffer half to the host and return immediately. */
static void submit_buffer(unsigned int offset, unsigned int len)
{
    AUDIO_BSTART = offset;
    AUDIO_BLEN   = len;
    AUDIO_CTRL   = 1;  /* host clears when queued to SDL */
}

/* Wait until host has consumed the pending buffer. */
static void wait_for_host(void)
{
    while (AUDIO_CTRL & 1)
        ;
}

static void poll_keyboard(int *freq, int *volume)
{
    while (KB_STATUS & 1)
    {
        unsigned int data = KB_DATA;
        unsigned int keycode = data & 0xFF;
        int pressed = (data & 0x100) ? 1 : 0;

        if (!pressed) continue;

        switch (keycode)
        {
            case 0x26: /* UP */
            case 'w':
                *freq += 50;
                if (*freq > 8000) *freq = 8000;
                break;
            case 0x28: /* DOWN */
            case 's':
                *freq -= 50;
                if (*freq < 50) *freq = 50;
                break;
            case 0x27: /* RIGHT */
            case 'd':
                *volume += 500;
                if (*volume > 16000) *volume = 16000;
                break;
            case 0x25: /* LEFT */
            case 'a':
                *volume -= 500;
                if (*volume < 200) *volume = 200;
                break;
        }
    }
}

void _start(void)
{
    printf("Sound demo: sine wave generator\n");
    printf("  W/UP = freq+  S/DOWN = freq-\n");
    printf("  D/RIGHT = vol+  A/LEFT = vol-\n");

    int freq = 440;
    int volume = 2000;
    unsigned int phase_acc = 0;

    /* Configure audio */
    AUDIO_SRATE = SAMPLE_RATE;
    AUDIO_CHAN  = 2;
    AUDIO_BITS  = 16;

    unsigned int lastPrintMs = RTC_MS_LO;

    /* Prime both buffers before starting playback */
    fill_sine(BUF1_OFFSET, freq, volume, &phase_acc);
    fill_sine(BUF2_OFFSET, freq, volume, &phase_acc);

    /* Submit buffer 1, start filling next into buffer 1 again */
    submit_buffer(BUF1_OFFSET, BUF_BYTES);
    int active = 0;  /* 0 = buf1 was just submitted, 1 = buf2 was just submitted */

    while (1)
    {
        poll_keyboard(&freq, &volume);

        /* Fill the idle buffer (the one NOT currently playing) */
        if (active == 0)
            fill_sine(BUF2_OFFSET, freq, volume, &phase_acc);
        else
            fill_sine(BUF1_OFFSET, freq, volume, &phase_acc);

        /* Wait for host to finish consuming the previously submitted buffer */
        wait_for_host();

        /* Submit the buffer we just filled */
        if (active == 0)
        {
            submit_buffer(BUF2_OFFSET, BUF_BYTES);
            active = 1;
        }
        else
        {
            submit_buffer(BUF1_OFFSET, BUF_BYTES);
            active = 0;
        }

        /* Print status every second */
        unsigned int now = RTC_MS_LO;
        if (now - lastPrintMs >= 1000)
        {
            printf("freq=%dHz vol=%d\n", freq, volume);
            lastPrintMs = now;
        }
    }
}
