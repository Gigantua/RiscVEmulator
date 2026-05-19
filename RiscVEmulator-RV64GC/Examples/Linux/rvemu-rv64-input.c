/*
 * rvemu-rv64-input.c — input bridge for the rvemu RV64 Alpine + XFCE build.
 *
 * The host KeyboardDevice / MouseDevice expose scancode + delta MMIO at
 * 0x10001000 / 0x10002000. X (libinput / evdev) wants /dev/input/event*.
 * This daemon mmaps the MMIO through /dev/mem, opens /dev/uinput, and
 * synthesizes a virtual keyboard+pointer device whose events X consumes.
 *
 * The keyboard FIFO already carries Linux KEY_* codes (see LinuxSdlViewer),
 * so they pass straight through as EV_KEY events. Cross-compiled static.
 */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

#define KBD_BASE   0x10001000UL
#define MOUSE_BASE 0x10002000UL

static volatile uint32_t *kbd;     /* [0]=has-data [1]=pop [2]=mods            */
static volatile uint32_t *mouse;   /* [0]=has-data [1]=dx [2]=dy [3]=buttons   */
static int ufd;

static void emit(int type, int code, int val)
{
    struct input_event ie;
    memset(&ie, 0, sizeof ie);
    ie.type = (uint16_t)type;
    ie.code = (uint16_t)code;
    ie.value = val;
    if (write(ufd, &ie, sizeof ie) < 0) { /* ignore */ }
}

int main(void)
{
    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) return 1;
    kbd   = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, KBD_BASE);
    mouse = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, MOUSE_BASE);
    if (kbd == MAP_FAILED || mouse == MAP_FAILED) return 2;

    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) return 3;

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_EVBIT, EV_REL);
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    for (int k = 1; k < 256; k++) ioctl(ufd, UI_SET_KEYBIT, k);
    ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(ufd, UI_SET_RELBIT, REL_X);
    ioctl(ufd, UI_SET_RELBIT, REL_Y);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    strcpy(us.name, "rvemu-input");
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x1234;
    us.id.product = 0x5678;
    ioctl(ufd, UI_DEV_SETUP, &us);
    ioctl(ufd, UI_DEV_CREATE, 0);

    uint32_t prevbtn = 0;
    for (;;)
    {
        /* keyboard — drain the scancode FIFO */
        int kev = 0;
        while (kbd[0])
        {
            uint32_t e = kbd[1];
            emit(EV_KEY, (int)(e & 0xFF), (e & 0x100) ? 1 : 0);
            kev = 1;
        }
        if (kev) emit(EV_SYN, SYN_REPORT, 0);

        /* mouse — relative deltas + buttons */
        int dx = (int)(int32_t)mouse[1];
        int dy = (int)(int32_t)mouse[2];
        uint32_t btn = mouse[3];
        uint32_t chg = btn ^ prevbtn;
        int mev = 0;
        if (dx) { emit(EV_REL, REL_X, dx); mev = 1; }
        if (dy) { emit(EV_REL, REL_Y, dy); mev = 1; }
        if (chg & 1) { emit(EV_KEY, BTN_LEFT,   btn & 1);        mev = 1; }
        if (chg & 2) { emit(EV_KEY, BTN_RIGHT,  (btn >> 1) & 1); mev = 1; }
        if (chg & 4) { emit(EV_KEY, BTN_MIDDLE, (btn >> 2) & 1); mev = 1; }
        prevbtn = btn;
        if (mev) emit(EV_SYN, SYN_REPORT, 0);

        usleep(8000);
    }
}
