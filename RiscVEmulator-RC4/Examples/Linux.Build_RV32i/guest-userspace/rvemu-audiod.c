/*
 * rvemu-audiod — userspace bridge from snd-aloop's loopback capture side to
 * the host AudioBufferDevice MMIO.
 *
 * This is the audio analogue of rvemu-input. snd-aloop registers a real
 * ALSA card "Loopback" with /dev/snd/pcmC0D0p (playback) routed internally
 * to /dev/snd/pcmC0D1c (capture). Any Linux app that opens "default"
 * (which our /etc/asound.conf maps to plug:hw:Loopback,0,0 at 44100 S16_LE
 * stereo) writes samples into D0p; the kernel forwards them to D1c. This
 * daemon reads from D1c via raw SNDRV_PCM_IOCTL_* ioctls — no libasound
 * dependency — and copies the frames into the AudioBufferDevice MMIO at
 * 0x30000000, then kicks AudioControlDevice at 0x30100000 to publish.
 * LinuxSdlAudio on the host drains the buffer to SDL2.
 *
 * Net effect: `aplay foo.wav`, `mpg123 song.mp3`, etc. play through the
 * SDL audio device on the host, with the kernel side looking like a
 * real soundcard to userspace (visible in `aplay -l`).
 *
 * Build (with the buildroot toolchain, same recipe as rvemu-input):
 *   $CC -static -fPIC -Wl,-elf2flt=-r -O2 rvemu-audiod.c -o rvemu-audiod
 *
 * Limitations (intentional, for a first cut):
 *   - Hardcoded 44100 Hz S16_LE stereo. asound.conf's `plug` resamples
 *     any app format down to this. Most non-default ALSA users will work
 *     too (mpg123 -a default, etc.).
 *   - Listens only to substream 0 of the loopback. Concurrent apps that
 *     land on substreams 1+ won't be heard until the first one closes.
 *   - On underrun/overrun the daemon re-PREPAREs and continues.
 */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sound/asound.h>

#define PCM_MMIO_BASE   0x30000000UL
#define PCM_MMIO_SIZE   0x100000UL
#define CTRL_MMIO_BASE  0x30100000UL
#define CTRL_MMIO_SIZE  0x1000UL

#define CTRL_CTRL        (0x00 / 4)
#define CTRL_SAMPLERATE  (0x08 / 4)
#define CTRL_CHANNELS    (0x0C / 4)
#define CTRL_BITDEPTH    (0x10 / 4)
#define CTRL_BUFSTART    (0x14 / 4)
#define CTRL_BUFLENGTH   (0x18 / 4)

#define LOOPBACK_DEV   "/dev/snd/pcmC0D1c"
#define SAMPLE_RATE    44100
#define CHANNELS       2
#define BYTES_PER_FRAME (CHANNELS * 2)         /* S16_LE × 2 ch = 4 bytes */
#define PERIOD_FRAMES  1024
#define PERIOD_BYTES   (PERIOD_FRAMES * BYTES_PER_FRAME)
#define BUFFER_FRAMES  4096

/* Two ping-pong MMIO slots so the host can drain one while the guest writes
 * the other. Each slot fits one period (1024 frames = 4 KB stereo). */
#define SLOT_BYTES   PERIOD_BYTES
#define SLOT_A       0
#define SLOT_B       PERIOD_BYTES

/* ── snd_pcm_hw_params helpers ─────────────────────────────────────────────
 *
 * The kernel UAPI doesn't ship param-set helpers (those live in libasound).
 * We do it by hand: a hw_params struct is "all masks 1, all intervals
 * 0..~0", then we narrow by setting specific masks/intervals before
 * SNDRV_PCM_IOCTL_HW_PARAMS. The kernel returns the negotiated values.
 */

static void hw_params_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    p->rmask = ~0U;
    p->cmask = 0;
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

static void hw_params_set_mask(struct snd_pcm_hw_params *p, int var, unsigned int val)
{
    int idx = var - SNDRV_PCM_HW_PARAM_FIRST_MASK;
    memset(&p->masks[idx], 0, sizeof(p->masks[idx]));
    p->masks[idx].bits[val >> 5] |= 1u << (val & 31);
}

static void hw_params_set_interval(struct snd_pcm_hw_params *p, int var, unsigned int val)
{
    int idx = var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    p->intervals[idx].min     = val;
    p->intervals[idx].max     = val;
    p->intervals[idx].integer = 1;
    p->intervals[idx].openmin = 0;
    p->intervals[idx].openmax = 0;
    p->intervals[idx].empty   = 0;
}

/* ── PCM lifecycle ─────────────────────────────────────────────────────── */

static int pcm_configure(int fd)
{
    struct snd_pcm_hw_params hw;
    hw_params_any(&hw);
    hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS,
                       SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT,
                       SNDRV_PCM_FORMAT_S16_LE);
    hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                       SNDRV_PCM_SUBFORMAT_STD);
    hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE,        SAMPLE_RATE);
    hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS,    CHANNELS);
    hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD_FRAMES);
    hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER_FRAMES);
    hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS,
                           BUFFER_FRAMES / PERIOD_FRAMES);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        fprintf(stderr, "rvemu-audiod: HW_PARAMS failed: %s\n", strerror(errno));
        return -1;
    }

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode      = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step      = 1;
    sw.avail_min        = PERIOD_FRAMES;
    sw.start_threshold  = 1;
    sw.stop_threshold   = BUFFER_FRAMES;
    sw.silence_threshold = 0;
    sw.silence_size      = 0;
    /* boundary must be a multiple of buffer_size; pick a comfortable
     * power-of-two so frame counters take a long time to wrap. */
    sw.boundary = BUFFER_FRAMES * 4096;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "rvemu-audiod: SW_PARAMS failed: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "rvemu-audiod: PREPARE failed: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
        /* For capture, START is sometimes redundant; ignore -EBADFD on
         * already-running streams. */
        if (errno != EBADFD) {
            fprintf(stderr, "rvemu-audiod: START failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* ── MMIO publish ──────────────────────────────────────────────────────── */

static void mmio_publish(volatile uint32_t *ctrl, uint8_t *pcm_mmio,
                         uint32_t slot, const void *data, size_t len)
{
    /* Wait for previous slot to drain (host clears Ctrl when queued to SDL). */
    struct timespec one_ms = { 0, 1 * 1000 * 1000 };
    int spins = 0;
    while (ctrl[CTRL_CTRL] & 1) {
        nanosleep(&one_ms, NULL);
        if (++spins > 500) {        /* 500 ms → host probably dead, drop */
            ctrl[CTRL_CTRL] = 0;
            break;
        }
    }
    memcpy(pcm_mmio + slot, data, len);
    ctrl[CTRL_BUFSTART]  = slot;
    ctrl[CTRL_BUFLENGTH] = (uint32_t)len;
    ctrl[CTRL_CTRL]      = 1;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

int main(void)
{
    const char hello[] = "rvemu-audiod: starting\n";
    (void)write(2, hello, sizeof(hello) - 1);

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("rvemu-audiod: /dev/mem"); return 1; }

    uint8_t *pcm_mmio = mmap(NULL, PCM_MMIO_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, mem, PCM_MMIO_BASE);
    if (pcm_mmio == MAP_FAILED) { perror("rvemu-audiod: mmap pcm"); return 1; }

    volatile uint32_t *ctrl = mmap(NULL, CTRL_MMIO_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mem, CTRL_MMIO_BASE);
    if ((void *)ctrl == MAP_FAILED) { perror("rvemu-audiod: mmap ctrl"); return 1; }

    /* Tell the host how to play back. Done once — every app rides the
     * fixed-format loopback so we never have to renegotiate. */
    ctrl[CTRL_SAMPLERATE] = SAMPLE_RATE;
    ctrl[CTRL_CHANNELS]   = CHANNELS;
    ctrl[CTRL_BITDEPTH]   = 16;

    /* Outer reopen loop — every time the loopback closes (app went away,
     * format changed) we close, reopen, reconfigure, keep going. */
    for (;;) {
        int pcm_fd = open(LOOPBACK_DEV, O_RDONLY);
        if (pcm_fd < 0) {
            fprintf(stderr, "rvemu-audiod: open(%s) failed: %s — sleeping\n",
                    LOOPBACK_DEV, strerror(errno));
            sleep(1);
            continue;
        }

        if (pcm_configure(pcm_fd) < 0) {
            close(pcm_fd);
            sleep(1);
            continue;
        }

        fprintf(stderr, "rvemu-audiod: loopback open, forwarding to MMIO\n");

        uint8_t frames[PERIOD_BYTES];
        uint32_t slot = SLOT_A;

        for (;;) {
            ssize_t n = read(pcm_fd, frames, PERIOD_BYTES);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EPIPE || errno == EBADFD || errno == ESTRPIPE) {
                    /* Underrun on capture (no app feeding playback side).
                     * Re-PREPARE and continue without reopening. */
                    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
                        fprintf(stderr, "rvemu-audiod: PREPARE on EPIPE: %s\n",
                                strerror(errno));
                        break;
                    }
                    continue;
                }
                fprintf(stderr, "rvemu-audiod: read failed: %s\n", strerror(errno));
                break;
            }
            if (n == 0) continue;
            mmio_publish(ctrl, pcm_mmio, slot, frames, (size_t)n);
            slot = (slot == SLOT_A) ? SLOT_B : SLOT_A;
        }

        close(pcm_fd);
        /* Brief pause before reopen to avoid tight loops if loopback is
         * permanently broken. */
        sleep(1);
    }
    /* not reached */
    return 0;
}
