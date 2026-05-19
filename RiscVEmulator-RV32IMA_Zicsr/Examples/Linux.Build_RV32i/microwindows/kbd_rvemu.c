/*
 * kbd_rvemu.c — Microwindows keyboard driver for the rvemu nommu guest.
 *
 * Reads raw evdev events from /dev/input/event0, which is published by
 * the rvemu-input userspace daemon (mmap'd MMIO at 0x10001000 → uinput).
 *
 * Adapted from the deprecated kbd_event.c (Davide Rizzo, 2008). The only
 * material change vs. the upstream is:
 *   - hardcoded /dev/input/event0 (no /sys/class/input probe — our daemon
 *     publishes the keyboard there and only there)
 *   - the upstream's "if (*modifiers)" guard is also corrected to "if (modifiers)"
 *
 * Built when KEYBOARD=RVEMU in src/config.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include "device.h"
#include "keymap_standard.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define RVEMU_KBD_DEV  "/dev/input/event0"

static int fd = -1;
static MWKEYMOD curmodif = 0, allmodif = 0;

static int
KBD_Open(KBDDEVICE *pkd)
{
    fd = open(RVEMU_KBD_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        EPRINTF("kbd_rvemu: open %s: %m\n", RVEMU_KBD_DEV);
        return errno ? errno : -1;
    }
    curmodif = 0;
    allmodif = MWKMOD_LSHIFT | MWKMOD_RSHIFT |
               MWKMOD_LCTRL  | MWKMOD_RCTRL  |
               MWKMOD_LALT   | MWKMOD_RALT   |
               MWKMOD_LMETA  | MWKMOD_RMETA  |
               MWKMOD_NUM    | MWKMOD_CAPS   |
               MWKMOD_ALTGR  | MWKMOD_SCR;
    return fd;
}

static void
KBD_Close(void)
{
    if (fd >= 0) close(fd);
    fd = -1;
}

static void
KBD_GetModifierInfo(MWKEYMOD *modifiers, MWKEYMOD *curmodifiers)
{
    curmodif &= allmodif;
    if (modifiers)    *modifiers    = allmodif;
    if (curmodifiers) *curmodifiers = curmodif;
}

static int
KBD_Read(MWKEY *buf, MWKEYMOD *modifiers, MWSCANCODE *pscancode)
{
    struct input_event ev;
    int n;

    while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev))
    {
        if (ev.type != EV_KEY)
            continue;

        if (ev.value) {
            switch (ev.code) {
            case KEY_LEFTSHIFT:  curmodif |= MWKMOD_LSHIFT; break;
            case KEY_RIGHTSHIFT: curmodif |= MWKMOD_RSHIFT; break;
            case KEY_LEFTCTRL:   curmodif |= MWKMOD_LCTRL;  break;
            case KEY_RIGHTCTRL:  curmodif |= MWKMOD_RCTRL;  break;
            case KEY_LEFTALT:    curmodif |= MWKMOD_LALT;   break;
            case KEY_RIGHTALT:   curmodif |= MWKMOD_RALT;   break;
            case KEY_LEFTMETA:   curmodif |= MWKMOD_LMETA;  break;
            case KEY_RIGHTMETA:  curmodif |= MWKMOD_RMETA;  break;
            case KEY_NUMLOCK:    curmodif |= MWKMOD_NUM;    break;
            case KEY_CAPSLOCK:   curmodif |= MWKMOD_CAPS;   break;
            case KEY_SCROLLLOCK: curmodif |= MWKMOD_SCR;    break;
            }
        } else {
            switch (ev.code) {
            case KEY_LEFTSHIFT:  curmodif &= ~MWKMOD_LSHIFT; break;
            case KEY_RIGHTSHIFT: curmodif &= ~MWKMOD_RSHIFT; break;
            case KEY_LEFTCTRL:   curmodif &= ~MWKMOD_LCTRL;  break;
            case KEY_RIGHTCTRL:  curmodif &= ~MWKMOD_RCTRL;  break;
            case KEY_LEFTALT:    curmodif &= ~MWKMOD_LALT;   break;
            case KEY_RIGHTALT:   curmodif &= ~MWKMOD_RALT;   break;
            case KEY_LEFTMETA:   curmodif &= ~MWKMOD_LMETA;  break;
            case KEY_RIGHTMETA:  curmodif &= ~MWKMOD_RMETA;  break;
            case KEY_NUMLOCK:    curmodif &= ~MWKMOD_NUM;    break;
            case KEY_CAPSLOCK:   curmodif &= ~MWKMOD_CAPS;   break;
            case KEY_SCROLLLOCK: curmodif &= ~MWKMOD_SCR;    break;
            }
        }

        if (modifiers) *modifiers = curmodif;

        if (ev.code < ARRAY_SIZE(keymap)) {
            *buf       = keymap[ev.code];
            *pscancode = ev.code;
            if (*buf == MWKEY_ESCAPE)
                return -2;
            /* 1 = press (auto-repeat counted as press), 2 = release */
            return ev.value ? 1 : 2;
        }
    }

    if (n == -1) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        EPRINTF("kbd_rvemu: read: %m\n");
        return -1;
    }
    if (n != 0) {
        EPRINTF("kbd_rvemu: short read (%d of %zu)\n", n, sizeof(ev));
        return -1;
    }
    return 0;
}

KBDDEVICE kbddev = {
    KBD_Open,
    KBD_Close,
    KBD_GetModifierInfo,
    KBD_Read,
    NULL
};
