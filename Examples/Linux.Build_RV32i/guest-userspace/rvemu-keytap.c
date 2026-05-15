/*
 * rvemu-keytap — read /dev/input/event0 and log each EV_KEY event to
 * /dev/console. End-to-end verification of the SDL→KeyboardDevice→MMIO→
 * uinput→evdev chain without depending on busybox `od`/`hexdump` builds.
 *
 * Runs alongside nano-X (read() on an evdev fd delivers each event to
 * every reader).
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <linux/input.h>

int main(void)
{
    int kbd = open("/dev/input/event0", O_RDONLY);
    int con = open("/dev/console",      O_WRONLY);
    if (kbd < 0 || con < 0) {
        dprintf(2, "rvemu-keytap: open failed (kbd=%d con=%d)\n", kbd, con);
        return 1;
    }
    dprintf(con, "[keytap] watching /dev/input/event0 (keycode = linux KEY_*)\n");

    struct input_event ev;
    while (read(kbd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        const char *st = ev.value == 0 ? "release"
                       : ev.value == 1 ? "press"
                       : "repeat";
        dprintf(con, "[keytap] key=%u %s\n", ev.code, st);
    }
    return 0;
}
