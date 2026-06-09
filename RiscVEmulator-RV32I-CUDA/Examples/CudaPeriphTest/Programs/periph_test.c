// periph_test.c — freestanding RV32I guest exercising every CUDA-backed
// memory-mapped peripheral. Reads pre-fed keyboard/mouse, draws to the
// framebuffer, fills the audio PCM buffer + control regs, emits a MIDI
// note, prints results over UART, and exits. No libc / runtime.

#include <stdint.h>

#define MMIO(a)  (*(volatile uint32_t*)(uintptr_t)(a))
#define MMIO8(a) (*(volatile uint8_t*)(uintptr_t)(a))

enum {
    UART = 0x10000000u, KBD = 0x10001000u, MOUSE = 0x10002000u, RTC = 0x10003000u,
    MIDI = 0x10005000u, FB = 0x20000000u, DISP = 0x20100000u,
    PCM = 0x30000000u, AUDIO = 0x30100000u, HOSTEXIT = 0x40000000u
};

// UART output ring (head counter + 2 KiB buffer in plain memory). The core just
// executes plain stores; the host drains the ring after each launch.
static void putc_(int c) {
    volatile uint32_t* head = (volatile uint32_t*)(uintptr_t)0x10000000u;
    volatile uint8_t*  ring = (volatile uint8_t*) (uintptr_t)0x10000800u;
    uint32_t h = *head;
    ring[h & 0x7FFu] = (uint8_t)c;
    *head = h + 1u;
}
static void puts_(const char* s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) {
    putc_('0'); putc_('x');
    for (int i = 28; i >= 0; i -= 4) { int d = (v >> i) & 0xF; putc_(d < 10 ? '0' + d : 'a' + d - 10); }
}

void _start(void) {
    // ── Keyboard (pre-fed by the host) ──
    if (MMIO(KBD + 0x00)) {
        uint32_t k = MMIO(KBD + 0x04), mod = MMIO(KBD + 0x08);
        puts_("KBD="); puthex(k); puts_(" MOD="); puthex(mod); putc_('\n');
    } else { puts_("KBD=none\n"); }

    // ── Mouse (pre-fed by the host) ──
    if (MMIO(MOUSE + 0x00)) {
        uint32_t dx = MMIO(MOUSE + 0x04), dy = MMIO(MOUSE + 0x08), b = MMIO(MOUSE + 0x0C);
        puts_("MOUSE dx="); puthex(dx); puts_(" dy="); puthex(dy); puts_(" b="); puthex(b); putc_('\n');
    } else { puts_("MOUSE=none\n"); }

    // ── RTC (host wall clock) ──
    puts_("RTC_ms="); puthex(MMIO(RTC + 0x08)); putc_('\n');

    // ── Framebuffer: known pattern in the first four pixels (direct FB) ──
    MMIO(FB + 0)  = 0x11223344; MMIO(FB + 4)  = 0x55667788;
    MMIO(FB + 8)  = 0xAABBCCDD; MMIO(FB + 12) = 0xDEADBEEF;
    MMIO(DISP + 0x1C) = 0;   // fbaddr = 0 → present the FB buffer directly
    MMIO(DISP + 0x0C) = 1;   // vsync

    // ── Present from a RAM shadow buffer (the path Voxel/Doom use) ──
    volatile uint32_t* shadow = (volatile uint32_t*)(uintptr_t)0x00800000u;
    for (int i = 0; i < 320 * 200; i++) shadow[i] = (uint32_t)(i * 7 + 0x100);
    MMIO(DISP + 0x1C) = 0x00800000u;  // fbaddr → RAM shadow
    MMIO(DISP + 0x0C) = 1;            // vsync: this becomes the presented frame

    // ── Audio: PCM payload + config + play ──
    for (int i = 0; i < 16; i++) MMIO8(PCM + i) = (uint8_t)(0x40 + i);
    MMIO(AUDIO + 0x08) = 44100;  // sample rate
    MMIO(AUDIO + 0x0C) = 2;      // channels
    MMIO(AUDIO + 0x10) = 16;     // bit depth
    MMIO(AUDIO + 0x14) = 0;      // buf start
    MMIO(AUDIO + 0x18) = 16;     // buf length
    MMIO(AUDIO + 0x00) = 1;      // CTRL: play

    // ── MIDI: note-on (status 0x90, note 0x3C, velocity 0x40) ──
    MMIO(MIDI + 0x04) = 0x00403C90u;

    // Vanilla RV32I: no M/A extensions. mul/div/rem are not part of the ISA
    // (the core traps them illegal); this peripheral test exercises devices only.

    puts_("DONE\n");
    MMIO(HOSTEXIT) = 0;          // exit code 0
    for (;;) { }
}
