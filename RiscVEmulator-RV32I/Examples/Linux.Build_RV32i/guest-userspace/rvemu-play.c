/*
 * rvemu-play — minimal ALSA player. Synthesises a sine tone and pushes it
 * through /dev/snd/pcmC0D0p (the loopback playback side) using raw
 * SNDRV_PCM_IOCTL_* — same path any standard Linux audio app takes.
 *
 * Exists so the audio stack can be end-to-end tested on a fresh boot
 * without pulling alsa-utils into the rootfs:
 *
 *   $ rvemu-play 440 2     # 440 Hz for 2 seconds
 *   $ rvemu-play           # default: 440 Hz for 1 second
 *
 * Build with the same recipe as rvemu-audiod / rvemu-input:
 *   $CC -static -fPIC -Wl,-elf2flt=-r -O2 rvemu-play.c -o rvemu-play
 */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#define PCM_DEV         "/dev/snd/pcmC0D0p"
#define SAMPLE_RATE     44100
#define CHANNELS        2
#define BYTES_PER_FRAME (CHANNELS * 2)
#define PERIOD_FRAMES   1024
#define BUFFER_FRAMES   4096

/* Same Q15 quarter-wave sine as rvemu-audiotest. Reused so this binary
 * still has zero softfloat / F-ext dependency. */
static const int16_t sine_q[257] = {
        0,   201,   402,   603,   804,  1005,  1206,  1407,
     1608,  1809,  2009,  2210,  2410,  2611,  2811,  3012,
     3212,  3412,  3612,  3811,  4011,  4210,  4410,  4609,
     4808,  5007,  5205,  5404,  5602,  5800,  5998,  6195,
     6393,  6590,  6786,  6983,  7179,  7375,  7571,  7767,
     7962,  8157,  8351,  8545,  8739,  8933,  9126,  9319,
     9512,  9704,  9896, 10087, 10278, 10469, 10659, 10849,
    11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353,
    12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
    15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673,
    16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357,
    19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631,
    20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22740, 22884, 23027,
    23170, 23311, 23452, 23592, 23731, 23870, 24007, 24143,
    24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198,
    26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
    27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803,
    28898, 28992, 29085, 29177, 29268, 29358, 29447, 29534,
    29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30425, 30499, 30572, 30644, 30714, 30784,
    30852, 30920, 30986, 31050, 31114, 31177, 31238, 31298,
    31357, 31415, 31471, 31527, 31581, 31634, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32056, 32097,
    32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382,
    32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717,
    32727, 32736, 32744, 32751, 32757, 32761, 32764, 32766,
    32767
};

static int16_t sine_lookup(uint32_t phase)
{
    uint32_t q = phase & 1023;
    if (q < 256)        return  sine_q[q];
    else if (q < 512)   return  sine_q[512 - q];
    else if (q < 768)   return (int16_t)-sine_q[q - 512];
    else                return (int16_t)-sine_q[1024 - q];
}

/* ── snd_pcm_hw_params helpers (mirror of rvemu-audiod's) ──────────────── */

static void hw_params_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    p->rmask = ~0U;
    p->info  = ~0U;
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK;
         n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        memset(m->bits, 0xFF, sizeof(m->bits));
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *iv =
            &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        iv->min = 0;
        iv->max = ~0U;
    }
}

static void set_mask(struct snd_pcm_hw_params *p, int var, unsigned int val)
{
    int idx = var - SNDRV_PCM_HW_PARAM_FIRST_MASK;
    memset(&p->masks[idx], 0, sizeof(p->masks[idx]));
    p->masks[idx].bits[val >> 5] |= 1u << (val & 31);
}

static void set_interval(struct snd_pcm_hw_params *p, int var, unsigned int val)
{
    int idx = var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    p->intervals[idx].min     = val;
    p->intervals[idx].max     = val;
    p->intervals[idx].integer = 1;
}

int main(int argc, char **argv)
{
    int hz   = (argc >= 2) ? atoi(argv[1]) : 440;
    int secs = (argc >= 3) ? atoi(argv[2]) : 1;
    if (hz   <= 0) hz   = 440;
    if (secs <= 0) secs = 1;

    int fd = open(PCM_DEV, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "rvemu-play: open(%s): %s\n", PCM_DEV, strerror(errno));
        fprintf(stderr, "  (is snd-aloop loaded? does rvemu-audiod need to be running?)\n");
        return 1;
    }

    struct snd_pcm_hw_params hw;
    hw_params_any(&hw);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS,    SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT,    SNDRV_PCM_FORMAT_S16_LE);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE,        SAMPLE_RATE);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS,    CHANNELS);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD_FRAMES);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER_FRAMES);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS,
                 BUFFER_FRAMES / PERIOD_FRAMES);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        fprintf(stderr, "rvemu-play: HW_PARAMS: %s\n", strerror(errno));
        return 1;
    }

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode     = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step     = 1;
    sw.avail_min       = PERIOD_FRAMES;
    sw.start_threshold = PERIOD_FRAMES;     /* start after one full period */
    sw.stop_threshold  = BUFFER_FRAMES;
    sw.boundary        = BUFFER_FRAMES * 4096;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "rvemu-play: SW_PARAMS: %s\n", strerror(errno));
        return 1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "rvemu-play: PREPARE: %s\n", strerror(errno));
        return 1;
    }

    int16_t period[PERIOD_FRAMES * CHANNELS];
    uint32_t phase = 0;
    uint32_t step  = ((uint32_t)hz * 1024u) / SAMPLE_RATE;

    int total_frames = SAMPLE_RATE * secs;
    int written = 0;

    fprintf(stderr, "rvemu-play: %d Hz for %d s (%d frames @ %d Hz stereo S16_LE)\n",
            hz, secs, total_frames, SAMPLE_RATE);

    while (written < total_frames) {
        int n = (total_frames - written) < PERIOD_FRAMES
              ? (total_frames - written)
              : PERIOD_FRAMES;
        for (int i = 0; i < n; i++) {
            int16_t s = sine_lookup(phase);
            period[i * 2 + 0] = s;
            period[i * 2 + 1] = s;
            phase += step;
        }
        ssize_t w = write(fd, period, (size_t)(n * BYTES_PER_FRAME));
        if (w < 0) {
            if (errno == EPIPE || errno == EBADFD) {
                fprintf(stderr, "rvemu-play: underrun, re-preparing\n");
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            fprintf(stderr, "rvemu-play: write: %s\n", strerror(errno));
            break;
        }
        written += (int)(w / BYTES_PER_FRAME);
    }

    /* Wait for the kernel to drain everything we've written. */
    ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
    close(fd);
    return 0;
}
