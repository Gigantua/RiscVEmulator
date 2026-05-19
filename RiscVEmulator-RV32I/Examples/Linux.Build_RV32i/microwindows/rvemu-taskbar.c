/*
 * rvemu-taskbar — bottom-of-screen launcher for the rvemu guest.
 *
 * Reads `.desktop`-style files from /etc/rvemu-launchers.d/ and shows one
 * button per launcher. Each file contains at minimum:
 *
 *   Name=Clock
 *   Exec=/usr/bin/nxclock
 *
 * The taskbar polls the directory's mtime every second — when a new file
 * appears (e.g. `rvpkg install` dropped one), the new button shows up
 * automatically without restarting. Polling beats inotify here because
 * busybox/uClibc nommu doesn't always have inotify wired.
 *
 * Launching is vfork() + execv() (works on nommu, no fork() needed).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "nano-X.h"

#define LAUNCHER_DIR  "/etc/rvemu-launchers.d"
#define MAX_BTNS      24
#define LABEL_LEN     16
#define PATH_LEN      96
#define BAR_H         32
#define BTN_W         72
#define BTN_GAP       4
#define BTN_TXT_PAD_X 8
#define POLL_MS       1000

#define BAR_BG     GR_RGB(60, 60, 70)
#define BTN_BG     GR_RGB(90, 100, 120)
#define BTN_BG_HOT GR_RGB(130, 150, 180)
#define BTN_BORDER GR_RGB(180, 190, 210)
#define TXT_FG     GR_RGB(240, 240, 240)

struct btn { char label[LABEL_LEN]; char prog[PATH_LEN]; };

static struct btn   btns[MAX_BTNS];
static int          nbtns;
static GR_WINDOW_ID bar;
static GR_GC_ID     gc;
static GR_SCREEN_INFO si;
static int          hot = -1;
static int          fheight;
static time_t       dir_mtime;

static int btn_cmp(const void *a, const void *b)
{
    return strcmp(((const struct btn *)a)->label, ((const struct btn *)b)->label);
}

static void
parse_desktop(const char *path)
{
    if (nbtns >= MAX_BTNS) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256], name[LABEL_LEN] = {0}, exec[PATH_LEN] = {0};
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *nl = strchr(val, '\n');
        if (nl) *nl = '\0';
        if (!strcmp(line, "Name")) strncpy(name, val, LABEL_LEN - 1);
        else if (!strcmp(line, "Exec")) strncpy(exec, val, PATH_LEN - 1);
    }
    fclose(f);
    if (!name[0] || !exec[0]) return;
    strncpy(btns[nbtns].label, name, LABEL_LEN - 1);
    strncpy(btns[nbtns].prog,  exec, PATH_LEN  - 1);
    nbtns++;
}

static void
scan_launchers(void)
{
    nbtns = 0;
    DIR *d = opendir(LAUNCHER_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && nbtns < MAX_BTNS) {
        const char *n = de->d_name;
        size_t l = strlen(n);
        if (l < 9 || strcmp(n + l - 8, ".desktop") != 0) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", LAUNCHER_DIR, n);
        parse_desktop(path);
    }
    closedir(d);
    qsort(btns, nbtns, sizeof(btns[0]), btn_cmp);
}

static int
dir_changed(void)
{
    struct stat st;
    if (stat(LAUNCHER_DIR, &st) < 0) return 0;
    if (st.st_mtime != dir_mtime) { dir_mtime = st.st_mtime; return 1; }
    return 0;
}

static int
btn_at(int x, int y)
{
    if (y < 0 || y >= BAR_H) return -1;
    for (int i = 0; i < nbtns; i++) {
        int bx = BTN_GAP + i * (BTN_W + BTN_GAP);
        if (x >= bx && x < bx + BTN_W) return i;
    }
    return -1;
}

static void
draw_button(int i)
{
    int bx = BTN_GAP + i * (BTN_W + BTN_GAP);
    int by = (BAR_H - 24) / 2;
    GrSetGCForeground(gc, i == hot ? BTN_BG_HOT : BTN_BG);
    GrFillRect(bar, gc, bx, by, BTN_W, 24);
    GrSetGCForeground(gc, BTN_BORDER);
    GrRect(bar, gc, bx, by, BTN_W, 24);
    GrSetGCForeground(gc, TXT_FG);
    GrSetGCBackground(gc, i == hot ? BTN_BG_HOT : BTN_BG);
    int tx = bx + BTN_TXT_PAD_X;
    int ty = by + (24 + fheight) / 2 - 2;
    GrText(bar, gc, tx, ty, btns[i].label, -1, GR_TFASCII);
}

static void
draw_bar(void)
{
    GrSetGCForeground(gc, BAR_BG);
    GrFillRect(bar, gc, 0, 0, si.cols, BAR_H);
    for (int i = 0; i < nbtns; i++) draw_button(i);
}

static void
launch(const char *prog)
{
    char *const argv[] = { (char*)prog, NULL };
    if (!vfork()) {
        execv(prog, argv);
        _exit(127);
    }
}

int main(void)
{
    if (GrOpen() < 0) { fprintf(stderr, "rvemu-taskbar: GrOpen failed\n"); return 1; }
    GrGetScreenInfo(&si);

    gc = GrNewGC();
    GrSetGCFont(gc, GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL));
    int fw, fb;
    GrGetGCTextSize(gc, "A", 1, GR_TFASCII, &fw, &fheight, &fb);

    scan_launchers();
    /* seed mtime so the first poll doesn't trigger an immediate rescan */
    struct stat st;
    if (stat(LAUNCHER_DIR, &st) == 0) dir_mtime = st.st_mtime;

    bar = GrNewWindowEx(
        GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NORESIZE | GR_WM_PROPS_NORAISE
        | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOFOCUS,
        "rvemu-taskbar", GR_ROOT_WINDOW_ID,
        0, si.rows - BAR_H, si.cols, BAR_H, BAR_BG);
    GrSelectEvents(bar,
        GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN |
        GR_EVENT_MASK_BUTTON_UP);
    GrMapWindow(bar);

    GR_EVENT ev;
    for (;;) {
        GrGetNextEventTimeout(&ev, POLL_MS);
        switch (ev.type) {
        case GR_EVENT_TYPE_TIMEOUT:
            /* poll for new .desktop files dropped by rvpkg */
            if (dir_changed()) {
                scan_launchers();
                hot = -1;
                draw_bar();
            }
            break;
        case GR_EVENT_TYPE_EXPOSURE:
            draw_bar();
            break;
        case GR_EVENT_TYPE_BUTTON_DOWN: {
            int i = btn_at(ev.button.x, ev.button.y);
            if (i >= 0) { hot = i; draw_button(i); }
            break;
        }
        case GR_EVENT_TYPE_BUTTON_UP: {
            int i = btn_at(ev.button.x, ev.button.y);
            int was_hot = hot;
            hot = -1;
            if (was_hot >= 0) draw_button(was_hot);
            if (i >= 0 && i == was_hot) launch(btns[i].prog);
            break;
        }
        }
    }
}
