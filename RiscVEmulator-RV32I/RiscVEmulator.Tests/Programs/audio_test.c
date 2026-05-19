/* audio_test.c — Tests audio buffer and control peripherals.
 * Writes a simple square wave to the audio buffer,
 * configures control registers, starts playback, verifies settings.
 */
#include <stddef.h>

#define UART_THR       (*(volatile char *)0x10000000)

/* Audio buffer: 1 MB at 0x30000000 */
#define AUDIO_BUF      ((volatile unsigned char *)0x30000000)

/* Audio control at 0x30100000 */
#define AUDIO_CTRL       (*(volatile unsigned int *)0x30100000)
#define AUDIO_STATUS     (*(volatile unsigned int *)0x30100004)
#define AUDIO_SRATE      (*(volatile unsigned int *)0x30100008)
#define AUDIO_CHANNELS   (*(volatile unsigned int *)0x3010000C)
#define AUDIO_BITDEPTH   (*(volatile unsigned int *)0x30100010)
#define AUDIO_BUF_START  (*(volatile unsigned int *)0x30100014)
#define AUDIO_BUF_LEN    (*(volatile unsigned int *)0x30100018)
#define AUDIO_POSITION   (*(volatile unsigned int *)0x3010001C)

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
    print_str("audio_test\n");

    /* Write a 256-byte square wave: 128 bytes of 0x7F, 128 bytes of 0x80 */
    for (unsigned int i = 0; i < 128; i++)
        AUDIO_BUF[i] = 0x7F;
    for (unsigned int i = 128; i < 256; i++)
        AUDIO_BUF[i] = 0x80;

    /* Read back to verify */
    check("buf_0", AUDIO_BUF[0] == 0x7F);
    check("buf_127", AUDIO_BUF[127] == 0x7F);
    check("buf_128", AUDIO_BUF[128] == 0x80);
    check("buf_255", AUDIO_BUF[255] == 0x80);

    /* Configure audio: 44100 Hz, stereo, 16-bit */
    AUDIO_SRATE = 44100;
    AUDIO_CHANNELS = 2;
    AUDIO_BITDEPTH = 16;
    AUDIO_BUF_START = 0;
    AUDIO_BUF_LEN = 256;

    check("srate", AUDIO_SRATE == 44100);
    check("channels", AUDIO_CHANNELS == 2);
    check("bitdepth", AUDIO_BITDEPTH == 16);
    check("buf_start", AUDIO_BUF_START == 0);
    check("buf_len", AUDIO_BUF_LEN == 256);

    /* Start playback */
    AUDIO_CTRL = 1;  /* play */
    check("playing", (AUDIO_STATUS & 1) != 0);

    /* Stop playback */
    AUDIO_CTRL = 4;  /* stop */
    check("stopped", (AUDIO_STATUS & 1) == 0);

    print_str("audio_done\n");

    /* Exit */
    *(volatile unsigned int *)0x40000000 = 0;   /* HostExit: write exit code -> halt */
    __builtin_unreachable();
}
