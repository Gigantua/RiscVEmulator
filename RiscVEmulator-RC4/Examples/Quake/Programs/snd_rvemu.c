/* snd_rvemu.c — Quake DMA-style sound driver against the rvemu emulator's
 * AudioBufferDevice (PCM @ 0x30000000) + AudioControlDevice (0x30100000).
 *
 * Quake's S_Update_ paints into shm->buffer every frame. We model `shm`
 * as a small static ring buffer in guest RAM; SNDDMA_GetDMAPos advances
 * by wall-clock × sample-rate so Quake's mixer keeps writing ahead of
 * the "play" cursor. SNDDMA_Submit copies the latest slice to the host
 * audio peripheral with the same non-blocking handshake the PureDOOM
 * port uses (skip the submit if the host is still draining the previous
 * buffer — `AUDIO_CTRL & 1`), at a fixed ~46 ms cadence.
 */

#include "quakedef.h"
#include "sound.h"

#define AUDIO_BUF     ((volatile short        *)0x30000000)
#define AUDIO_CTRL    (*(volatile unsigned int *)0x30100000)
#define AUDIO_SRATE   (*(volatile unsigned int *)0x30100008)
#define AUDIO_CHAN    (*(volatile unsigned int *)0x3010000C)
#define AUDIO_BITS    (*(volatile unsigned int *)0x30100010)
#define AUDIO_BSTART  (*(volatile unsigned int *)0x30100014)
#define AUDIO_BLEN    (*(volatile unsigned int *)0x30100018)

#define RTC_US_LO     (*(volatile unsigned int *)0x10003000)
#define RTC_US_HI     (*(volatile unsigned int *)0x10003004)

#define MIX_RATE      11025
#define MIX_CHANNELS  2
#define MIX_BITS      16
/* Ring-buffer size in samples (across channels). Power of two so wrap
 * is a cheap mask. ~743 ms of audio at 11025 Hz × 2 chans. */
#define RING_SAMPLES  16384

static short  ring[RING_SAMPLES * MIX_CHANNELS];
static dma_t  the_shm;
extern dma_t *shm;

static unsigned long long
us_now(void)
{
    unsigned int lo = RTC_US_LO;
    unsigned int hi = RTC_US_HI;
    return ((unsigned long long)hi << 32) | lo;
}

qboolean
SNDDMA_Init(void)
{
    AUDIO_SRATE = MIX_RATE;
    AUDIO_CHAN  = MIX_CHANNELS;
    AUDIO_BITS  = MIX_BITS;

    the_shm.speed            = MIX_RATE;
    the_shm.channels         = MIX_CHANNELS;
    the_shm.samplebits       = MIX_BITS;
    the_shm.samples          = RING_SAMPLES * MIX_CHANNELS;
    the_shm.samplepos        = 0;
    the_shm.buffer           = (unsigned char *)ring;
    the_shm.submission_chunk = 1;
    shm = &the_shm;
    return true;
}

/* Advance samplepos by wall-clock × sample-rate × channels. Quake's mixer
 * uses this to decide how many fresh samples to paint each call — it must
 * be monotonically increasing modulo the ring size. */
int
SNDDMA_GetDMAPos(void)
{
    static unsigned long long start_us = 0;
    if (!start_us) start_us = us_now();
    unsigned long long elapsed = us_now() - start_us;
    /* samples = us × rate / 1,000,000, then ×channels for stereo addressing. */
    unsigned long long samples =
        (elapsed * MIX_RATE) / 1000000ull * MIX_CHANNELS;
    the_shm.samplepos = (int)(samples % (unsigned long long)the_shm.samples);
    return the_shm.samplepos;
}

void
SNDDMA_Shutdown(void)
{
    AUDIO_CTRL = 0;
    shm = 0;
}

/* Copy the ring to the host audio buffer at a fixed cadence, but only if
 * the host has finished playing the previous slice (`AUDIO_CTRL & 1`).
 * Submitting while busy stomps an in-flight buffer and causes the glitchy
 * "audio is very broken" symptom. */
void
SNDDMA_Submit(void)
{
    static unsigned long long last_submit_us = 0;
    unsigned long long now = us_now();
    /* ~512 samples at 11 025 Hz = ~46 ms; matches PureDOOM. */
    if (now - last_submit_us < 46000ull) return;
    if (AUDIO_CTRL & 1u) return;     /* host still busy with prev slice */
    last_submit_us = now;

    /* Submit a 512-sample × 2-channel chunk starting at the current play
     * head. The host plays this as one contiguous PCM segment. */
    const int nsamples = 512 * MIX_CHANNELS;
    int pos = the_shm.samplepos & ~1; /* align to stereo frame */
    volatile short *dst = AUDIO_BUF;
    for (int i = 0; i < nsamples; i++) {
        dst[i] = ring[(pos + i) % the_shm.samples];
    }
    AUDIO_BSTART = 0;
    AUDIO_BLEN   = (unsigned int)(nsamples * (MIX_BITS / 8));
    AUDIO_CTRL   = 1;
}

int  SNDDMA_LockBuffer(void)   { return 0; }
void SNDDMA_UnlockBuffer(void) { }
void S_BlockSound(void)        { }
