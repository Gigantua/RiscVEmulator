/*
 * rvemu-midid — bridge from snd-virmidi rawmidi to the host MidiDevice MMIO.
 *
 * snd-virmidi (CONFIG_SND_VIRMIDI=y) registers a virtual ALSA rawmidi card.
 * Each device /dev/snd/midiCxD0..3 loops writes through the alsa-sequencer
 * subsystem to readers of the same device. This daemon opens the first
 * available device for read, parses the raw MIDI byte stream (with running-
 * status handling), packs each complete short message into a uint32_t and
 * writes it to the MidiDevice MMIO register at 0x10005004. The host
 * MidiDevice forwards each via winmm.midiOutShortMsg() to the default
 * Windows GM synth (built into Windows; no extra software needed).
 *
 * Net effect: any ALSA app that opens a rawmidi device (amidi, aplaymidi,
 * timidity, fluidsynth) or any sequencer client that connects to virmidi's
 * seq port plays through the Windows MIDI synth.
 *
 * Build (with the buildroot toolchain, same recipe as rvemu-audiod):
 *   $CC -static -fPIC -Wl,-elf2flt=-r -O2 rvemu-midid.c -o rvemu-midid
 *
 * Quick test from a guest shell:
 *   amidi -p hw:1,0 --send-hex='90 3C 7F'   # Note On, middle C, full vel
 *   sleep 1
 *   amidi -p hw:1,0 --send-hex='80 3C 00'   # Note Off
 *
 * Limitations (intentional, v1):
 *   - SysEx (status 0xF0..0xF7) is dropped. winmm.midiOutShortMsg is short-
 *     message-only; supporting SysEx requires extending MidiDevice with a
 *     buffered long-message path (midiOutLongMsg + MIDIHDR). Deferred.
 *   - Bridges only the first /dev/snd/midiC*D0 found. Apps that target D1+
 *     are silent until rebound. snd-virmidi creates 4 devices; binding all
 *     four would need 4 fds + a poll() loop — trivial extension if needed.
 *   - Realtime messages (0xF8..0xFF) pass through immediately as 1-byte
 *     short messages, even mid-message, per the MIDI spec.
 */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MIDI_MMIO_BASE  0x10005000UL
#define MIDI_MMIO_SIZE  0x1000UL

/* MidiDevice register layout (see Core/Peripherals/MidiDevice.cs):
 *   0x00  status (read: 1 if a synth is open, else 0)
 *   0x04  short-message dword (write only): packed (data2<<16)|(data1<<8)|status
 *   0x08  reset             (write 1 → midiOutReset)
 *   0x0C  sleep-ms          (write N → host sleeps N ms; not used here)        */
#define MIDI_REG_SHORT  (0x04 / 4)

/* short_msg_len — given a status byte, return the total length (status+data)
 * of the short message, or 0 if not a short-message status. Variable-length
 * SysEx (0xF0/0xF7) and reserved statuses return 0. */
static int short_msg_len(uint8_t status)
{
    if (status < 0x80) return 0;
    switch (status & 0xF0) {
        case 0x80:                          /* Note Off               */
        case 0x90:                          /* Note On                */
        case 0xA0:                          /* Polyphonic Aftertouch  */
        case 0xB0:                          /* Control Change         */
        case 0xE0:                          /* Pitch Bend             */
            return 3;
        case 0xC0:                          /* Program Change         */
        case 0xD0:                          /* Channel Aftertouch     */
            return 2;
        case 0xF0:
            switch (status) {
                case 0xF1: return 2;        /* MTC quarter-frame      */
                case 0xF2: return 3;        /* Song Position Pointer  */
                case 0xF3: return 2;        /* Song Select            */
                case 0xF6: return 1;        /* Tune Request           */
                case 0xF8: return 1;        /* Timing Clock           */
                case 0xFA: return 1;        /* Start                  */
                case 0xFB: return 1;        /* Continue               */
                case 0xFC: return 1;        /* Stop                   */
                case 0xFE: return 1;        /* Active Sensing         */
                case 0xFF: return 1;        /* System Reset           */
                /* 0xF0 SysEx start / 0xF7 SysEx end → variable; not short. */
                default:   return 0;        /* 0xF4, 0xF5, 0xF9, 0xFD reserved */
            }
    }
    return 0;
}

/* Card order is: snd-aloop = 0 (registered via CONFIG_SND_ALOOP for audio),
 * snd-virmidi = 1. Probe a small range to be robust against reordering. */
static int open_first_midi(void)
{
    char path[32];
    for (int c = 0; c < 8; c++) {
        snprintf(path, sizeof(path), "/dev/snd/midiC%dD0", c);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            fprintf(stderr, "rvemu-midid: reading from %s\n", path);
            return fd;
        }
    }
    return -1;
}

int main(void)
{
    const char hello[] = "rvemu-midid: starting\n";
    (void)write(2, hello, sizeof(hello) - 1);

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("rvemu-midid: /dev/mem"); return 1; }

    volatile uint32_t *midi = mmap(NULL, MIDI_MMIO_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mem, MIDI_MMIO_BASE);
    if ((void *)midi == MAP_FAILED) {
        perror("rvemu-midid: mmap MIDI MMIO");
        return 1;
    }

    /* Outer reopen loop: if the rawmidi descriptor goes away (driver
     * reload, app crash, etc.) wait briefly and try again. */
    for (;;) {
        int fd = open_first_midi();
        if (fd < 0) {
            fprintf(stderr,
                "rvemu-midid: no /dev/snd/midiC*D0 found, sleeping 2s\n");
            sleep(2);
            continue;
        }

        uint8_t status = 0;       /* most recent status (for running status)   */
        uint8_t buf[3];           /* assembling current short message          */
        int     filled = 0;       /* bytes accumulated into buf                */
        int     needed = 0;       /* short_msg_len(status); 0 = no status yet  */
        int     in_sysex = 0;     /* true while we're skipping a SysEx blob    */

        for (;;) {
            uint8_t b;
            ssize_t n = read(fd, &b, 1);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr,
                    "rvemu-midid: read failed: %s\n", strerror(errno));
                break;
            }
            if (n == 0) break;    /* EOF — device went away, reopen */

            /* Realtime messages (0xF8..0xFF) may appear anywhere in the
             * stream, even mid-message. Emit immediately and don't touch
             * the running-status state. */
            if (b >= 0xF8) {
                midi[MIDI_REG_SHORT] = b;
                continue;
            }

            if (b & 0x80) {
                /* Status byte. Per MIDI spec, any non-realtime status
                 * implicitly terminates an in-progress SysEx. */
                if (in_sysex && b != 0xF7) in_sysex = 0;

                if (b == 0xF0) { in_sysex = 1; continue; }
                if (b == 0xF7) { in_sysex = 0; continue; }

                status = b;
                needed = short_msg_len(status);
                buf[0] = status;
                filled = 1;
            } else {
                /* Data byte. */
                if (in_sysex)  continue;
                if (needed == 0) continue;          /* no status established */
                if (filled == 0) {
                    /* Running status: implicit status byte from previous msg. */
                    buf[0] = status;
                    filled = 1;
                }
                if (filled < 3) buf[filled] = b;
                filled++;
            }

            if (needed > 0 && filled >= needed) {
                uint32_t pack = (uint32_t)buf[0];
                if (needed >= 2) pack |= (uint32_t)buf[1] << 8;
                if (needed >= 3) pack |= (uint32_t)buf[2] << 16;
                midi[MIDI_REG_SHORT] = pack;
                /* Keep `status` and `needed` for running-status continuation;
                 * clear `filled` so the next data byte triggers the fill-in. */
                filled = 0;
            }
        }

        close(fd);
        sleep(1);
    }
    /* unreachable */
    return 0;
}
