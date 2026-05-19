/*
 * rvemu-audiotest — minimal PCM audio demo proving the guest → host audio
 * path works end-to-end.
 *
 *   1. mmaps the PCM buffer at 0x30000000 (1 MB plain RAM) and the audio
 *      control registers at 0x30100000 (guarded MMIO) via /dev/mem.
 *   2. Configures the host SDL output for 22050 Hz mono S16_LE.
 *   3. Writes a sine wave one chunk at a time, kicks Ctrl|=1 to publish,
 *      then spins until the host clears Ctrl to indicate the slot is free.
 *
 * Listen for it with `--gui` on the host emulator; LinuxSdlAudio will open
 * an SDL audio device and you should hear a 440 Hz tone.
 *
 * Build with the same recipe as rvemu-input / rvemu-fbtest:
 *   ${CC} -static -fPIC -Wl,-elf2flt=-r -O2 rvemu-audiotest.c -o rvemu-audiotest
 *
 * Register layout (AudioControlDevice.cs):
 *   0x00 Ctrl       (bit0=play, bit2=reset). Host clears bit0 when drained.
 *   0x04 Status     (read-only, 1 if playing)
 *   0x08 SampleRate (Hz)
 *   0x0C Channels   (1 or 2)
 *   0x10 BitDepth   (always 16 for now)
 *   0x14 BufStart   (byte offset into the PCM buffer at 0x30000000)
 *   0x18 BufLength  (byte length of the slice to play)
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PCM_BASE   0x30000000UL
#define PCM_SIZE   0x100000UL
#define CTRL_BASE  0x30100000UL
#define CTRL_SIZE  0x1000UL

#define CTRL_CTRL        (0x00 / 4)
#define CTRL_SAMPLERATE  (0x08 / 4)
#define CTRL_CHANNELS    (0x0C / 4)
#define CTRL_BITDEPTH    (0x10 / 4)
#define CTRL_BUFSTART    (0x14 / 4)
#define CTRL_BUFLENGTH   (0x18 / 4)

#define SAMPLE_RATE  22050
#define TONE_HZ      440
#define CHUNK_SAMPLES 2205     /* 100 ms at 22050 Hz */

/* Two ping-pong slots inside the 1 MB PCM buffer so the host always has
 * a clean slice to read from while the guest fills the other one. */
#define SLOT_BYTES  (CHUNK_SAMPLES * 2)
#define SLOT_A      0
#define SLOT_B      (SLOT_BYTES)

/* Fixed-point sine — keeps this binary free of softfloat (no -lm, no F-ext
 * required just for a tone generator). 16-bit Q15 cosine table, quarter
 * wave, mirrored. */
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

/* Q15 sine for an arbitrary phase in [0, 1024). Maps to the quarter-wave
 * table by quadrant. */
static int16_t sine_lookup(uint32_t phase)
{
    uint32_t q = phase & 1023;
    if (q < 256)        return  sine_q[q];
    else if (q < 512)   return  sine_q[512 - q];
    else if (q < 768)   return (int16_t)-sine_q[q - 512];
    else                return (int16_t)-sine_q[1024 - q];
}

int main(void)
{
    const char hi[] = "rvemu-audiotest: starting (440 Hz tone)\n";
    (void)write(2, hi, sizeof(hi) - 1);

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("/dev/mem"); return 1; }

    int16_t *pcm = mmap(NULL, PCM_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, mem, PCM_BASE);
    if (pcm == MAP_FAILED) { perror("mmap pcm"); return 1; }

    volatile uint32_t *ctrl = mmap(NULL, CTRL_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mem, CTRL_BASE);
    if ((void *)ctrl == MAP_FAILED) { perror("mmap ctrl"); return 1; }

    /* Configure the host audio stream once. Don't set Ctrl yet — that has
     * to happen AFTER the first buffer is filled so the drain doesn't pick
     * up stale (zero) data. */
    ctrl[CTRL_SAMPLERATE] = SAMPLE_RATE;
    ctrl[CTRL_CHANNELS]   = 1;
    ctrl[CTRL_BITDEPTH]   = 16;

    /* phase in fixed-point cycles (1024 units per full cycle). step = how
     * much phase to advance per sample to land on TONE_HZ. */
    uint32_t phase = 0;
    uint32_t step  = (TONE_HZ * 1024u) / SAMPLE_RATE;
    uint32_t slot  = SLOT_A;

    struct timespec one_ms = { 0, 1 * 1000 * 1000 };

    for (;;) {
        int16_t *out = (int16_t *)((uint8_t *)pcm + slot);
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            out[i] = sine_lookup(phase);
            phase += step;
        }

        /* Publish: BufStart/Length first, then Ctrl|=1 (= WriteGeneration++
         * on the host side). The host clears Ctrl once it has queued the
         * slice — at which point we're free to refill the other slot. */
        ctrl[CTRL_BUFSTART]  = slot;
        ctrl[CTRL_BUFLENGTH] = SLOT_BYTES;
        ctrl[CTRL_CTRL]      = 1;

        while (ctrl[CTRL_CTRL] & 1)
            nanosleep(&one_ms, NULL);

        slot = (slot == SLOT_A) ? SLOT_B : SLOT_A;
    }
    return 0;
}
