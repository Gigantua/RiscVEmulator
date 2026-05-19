/* rvemu-rv64-audio — guest daemon bridging ALSA PCM to the rvemu audio MMIO.
 *
 * The rv64 kernel has no sound subsystem, so there is no ALSA card. Instead
 * /etc/asound.conf routes the default PCM through alsa-lib's pure-userspace
 * `file` plugin, which writes raw 44100 Hz / 2ch / S16_LE PCM into a FIFO.
 * This daemon reads that FIFO and pushes the PCM into the emulator's audio
 * device:  buffer at 0x30000000, control regs at 0x30100000 (see
 * Core/Peripherals/AudioDevice.cs). The host LinuxSdlAudio drains it to SDL.
 *
 * Build (static, runs on musl Alpine):
 *   riscv64-buildroot-linux-gnu-gcc -static -O2 rvemu-rv64-audio.c \
 *       -o rvemu-rv64-audio
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PCM_BASE 0x30000000UL
#define PCM_SIZE 0x100000UL          /* 1 MB PCM ring */
#define CTL_BASE 0x30100000UL
#define CTL_SIZE 0x1000UL
#define FIFO     "/tmp/rvemu-audio.fifo"
#define CHUNK    4096                /* bytes pushed per slot */

/* AudioControlDevice register indices (32-bit words) */
#define R_CTRL   (0x00/4)
#define R_SRATE  (0x08/4)
#define R_CHAN   (0x0C/4)
#define R_BITS   (0x10/4)
#define R_BSTART (0x14/4)
#define R_BLEN   (0x18/4)

int main(void)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("rvemu-audio: open /dev/mem"); return 1; }

    volatile uint8_t *pcm = mmap(0, PCM_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, memfd, PCM_BASE);
    volatile uint32_t *ctl = mmap(0, CTL_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, memfd, CTL_BASE);
    if (pcm == MAP_FAILED || ctl == MAP_FAILED) {
        perror("rvemu-audio: mmap"); return 1;
    }

    mkfifo(FIFO, 0666);
    fprintf(stderr, "rvemu-audio: ready, reading %s\n", FIFO);

    static uint8_t chunk[CHUNK];
    for (;;) {
        /* blocks until an ALSA client opens the FIFO for writing */
        int fd = open(FIFO, O_RDONLY);
        if (fd < 0) { sleep(1); continue; }

        int got = 0;
        for (;;) {
            ssize_t n = read(fd, chunk + got, CHUNK - got);
            if (n > 0) {
                got += n;
                if (got < CHUNK) continue;       /* keep filling the slot */
            }
            if (got > 0) {
                /* wait (briefly) for the host to free the previous slot */
                int spins = 0;
                while ((ctl[R_CTRL] & 1u) && ++spins < 200000) { }
                memcpy((void *)pcm, chunk, got);
                ctl[R_SRATE]  = 44100;
                ctl[R_CHAN]   = 2;
                ctl[R_BITS]   = 16;
                ctl[R_BSTART] = 0;
                ctl[R_BLEN]   = (uint32_t)got;
                ctl[R_CTRL]   = 1;               /* play this slot */
                got = 0;
            }
            if (n <= 0) break;                   /* writer closed the FIFO */
        }
        close(fd);
    }
}
