/*
 * rvemu-input — userspace bridge between rvemu's MMIO keyboard/mouse and the
 * Linux input subsystem.
 *
 * Polls KeyboardDevice (0x10001000) and MouseDevice (0x10002000) via mmap
 * of /dev/mem, translates the events, and pushes them into /dev/uinput so
 * Linux exposes them as /dev/input/event0 + event1. After this daemon is
 * running, any standard fbdev/evdev app (Microwindows, fbterm, weston-tiny,
 * ...) sees keyboard+mouse as native Linux input devices.
 *
 * Build (cross-compile, run from WSL with the buildroot toolchain in PATH):
 *
 *   ~/rvemu-buildroot/output/host/bin/riscv32-buildroot-linux-uclibc-gcc \
 *       -static -O2 -Wall rvemu-input.c -o rvemu-input
 *
 * Then copy `rvemu-input` into the buildroot overlay at
 * `board/rvemu/rootfs-overlay/usr/bin/rvemu-input` and a small init.d
 * script `S42input` to launch it after networking.
 *
 * Requires CONFIG_INPUT_UINPUT=y in the kernel (added by
 * Examples.Linux.Build_RV32i alongside CONFIG_INPUT_EVDEV).
 *
 * Keyboard register layout (KeyboardDevice.cs):
 *   0x00  status:  1 if FIFO has data, 0 if empty.
 *   0x04  pop:     scancode | (pressed << 8). Reads consume one entry.
 *   0x08  modifiers bitmask (shift=1, ctrl=2, alt=4).
 *
 * Mouse register layout (MouseDevice.cs):
 *   0x00  status:  1 if any deltas/buttons pending.
 *   0x04  dx:      signed 32-bit delta-x. Reads clear the field.
 *   0x08  dy:      signed 32-bit delta-y. Reads clear the field.
 *   0x0C  buttons: bit0=left, bit1=right, bit2=middle.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define KBD_BASE      0x10001000UL
#define MOUSE_BASE    0x10002000UL

#define KBD_STATUS    (0x00 / 4)
#define KBD_DATA      (0x04 / 4)

#define MOUSE_STATUS  (0x00 / 4)
#define MOUSE_DX      (0x04 / 4)
#define MOUSE_DY      (0x08 / 4)
#define MOUSE_BUTTONS (0x0C / 4)

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type  = type;
    ie.code  = code;
    ie.value = value;
    (void)write(fd, &ie, sizeof(ie));
}

static int make_kbd_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "rvemu-input: kbd open(/dev/uinput) failed\n"); return -1; }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) { fprintf(stderr, "rvemu-input: kbd SET_EVBIT failed\n"); close(fd); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    /* Bound the keycode loop — KEY_MAX is huge and lots of codes are reserved.
     * 256 covers all standard Linux keys (KEY_RESERVED..KEY_MICMUTE-ish). */
    for (int k = 1; k < 256; k++) ioctl(fd, UI_SET_KEYBIT, k);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "rvemu-keyboard");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor  = 0x1234;
    dev.id.product = 0x0001;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof(dev)) != sizeof(dev)) {
        fprintf(stderr, "rvemu-input: kbd uinput_user_dev write failed\n");
        close(fd); return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "rvemu-input: kbd UI_DEV_CREATE failed\n");
        close(fd); return -1;
    }
    fprintf(stderr, "rvemu-input: kbd uinput device created (fd %d)\n", fd);
    return fd;
}

static int make_mouse_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "rvemu-mouse");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor  = 0x1234;
    dev.id.product = 0x0002;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof(dev)) != sizeof(dev)) { close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0)                { close(fd); return -1; }
    return fd;
}

int main(void)
{
    /* Direct write to fd 2 so we know main() actually ran even if uclibc
     * stdio init is broken. Without this we couldn't tell the difference
     * between "BFLT failed to load" and "stdio never initialised". */
    const char sentinel[] = "rvemu-input: main entered\n";
    (void)write(2, sentinel, sizeof(sentinel) - 1);

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) { perror("open /dev/mem"); return 1; }

    volatile uint32_t *kbd =
        mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem, KBD_BASE);
    if (kbd == MAP_FAILED) { perror("mmap kbd"); return 1; }

    volatile uint32_t *mouse =
        mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem, MOUSE_BASE);
    if (mouse == MAP_FAILED) { perror("mmap mouse"); return 1; }

    fprintf(stderr, "rvemu-input: starting\n");
    int kbd_fd   = make_kbd_uinput();
    int mouse_fd = make_mouse_uinput();
    if (kbd_fd < 0 || mouse_fd < 0) {
        fprintf(stderr, "rvemu-input: failed to create uinput devices\n");
        return 1;
    }
    fprintf(stderr, "rvemu-input: entering poll loop\n");

    uint32_t prev_buttons = 0;
    struct timespec poll = { 0, 10 * 1000 * 1000 };   /* 10 ms = 100 Hz */

    for (;;) {
        /* Drain keyboard FIFO. */
        while (kbd[KBD_STATUS]) {
            uint32_t e        = kbd[KBD_DATA];
            uint8_t  scancode = (uint8_t)(e & 0xFF);
            int      pressed  = (e >> 8) & 1;
            emit(kbd_fd, EV_KEY, scancode, pressed ? 1 : 0);
            emit(kbd_fd, EV_SYN, SYN_REPORT, 0);
        }

        /* Drain mouse state. */
        if (mouse[MOUSE_STATUS]) {
            int32_t  dx  = (int32_t)mouse[MOUSE_DX];
            int32_t  dy  = (int32_t)mouse[MOUSE_DY];
            uint32_t btn = mouse[MOUSE_BUTTONS];

            if (dx) emit(mouse_fd, EV_REL, REL_X, dx);
            if (dy) emit(mouse_fd, EV_REL, REL_Y, dy);

            uint32_t changed = btn ^ prev_buttons;
            if (changed & 1) emit(mouse_fd, EV_KEY, BTN_LEFT,   btn & 1);
            if (changed & 2) emit(mouse_fd, EV_KEY, BTN_RIGHT,  (btn >> 1) & 1);
            if (changed & 4) emit(mouse_fd, EV_KEY, BTN_MIDDLE, (btn >> 2) & 1);
            prev_buttons = btn;

            emit(mouse_fd, EV_SYN, SYN_REPORT, 0);
        }

        nanosleep(&poll, NULL);
    }
    return 0;
}
