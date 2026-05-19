/*
 * rvemu-term — a small Microwindows nano-X terminal-like client.
 *
 * Spawns busybox sh -i over a Unix98 pty so isatty()-checking apps
 * (nano, vi, less, top) work. Falls back to a socketpair if no pty is
 * available — then only line-oriented programs work, but the window
 * still functions.
 *
 * Not a full vt100 emulator — control characters are mostly passed
 * through with a small set of handlers (\n \r \b \t). Apps that paint
 * with escape sequences (nano's status bar, ncurses) will draw stray
 * glyphs; basic shells, cat, grep, ls, head are fine.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include "nano-X.h"

#define COLS    80
#define ROWS    24
#define CW      8      /* SYSTEM_FIXED glyph width  */
#define CH      13     /* SYSTEM_FIXED glyph height */
#define WIN_W   (COLS * CW + 8)
#define WIN_H   (ROWS * CH + 8)

#define COL_BG     GR_RGB(10, 20, 10)
#define COL_FG     GR_RGB(200, 230, 200)
#define COL_CARET  GR_RGB(120, 220, 120)

static char        screen[ROWS][COLS];
static int         cur_r, cur_c;

/* Minimal VT100 / xterm escape-sequence parser. We only handle the
 * subset that busybox's line-editor produces (cursor motion, erase
 * to end of line / screen, SGR colors — colors are ignored). Without
 * this, `backspace` on a pty produces `\e[D \e[K` which would print
 * as literal text. */
enum esc_state { ESC_NONE, ESC_GOT_ESC, ESC_CSI };
static int esc_state_;
static int esc_p_[8];      /* CSI numeric params */
static int esc_n_;
static int esc_p_active_;  /* did the current param get any digits? */
static GR_WINDOW_ID win;
static GR_GC_ID    gc;
static int         sh_fd = -1;
static pid_t       sh_pid = -1;

static void
clear_screen(void)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            screen[r][c] = ' ';
    cur_r = cur_c = 0;
}

static void
scroll_up(void)
{
    for (int r = 0; r < ROWS - 1; r++)
        memcpy(screen[r], screen[r + 1], COLS);
    memset(screen[ROWS - 1], ' ', COLS);
    if (cur_r > 0) cur_r--;
}

static void
draw_all(void)
{
    GrSetGCForeground(gc, COL_BG);
    GrFillRect(win, gc, 0, 0, WIN_W, WIN_H);
    GrSetGCForeground(gc, COL_FG);
    GrSetGCBackground(gc, COL_BG);
    for (int r = 0; r < ROWS; r++) {
        /* Need a writable copy because GrText doesn't take const. */
        char line[COLS + 1];
        memcpy(line, screen[r], COLS);
        line[COLS] = '\0';
        GrText(win, gc, 4, 4 + (r + 1) * CH - 2, line, COLS, GR_TFASCII);
    }
    /* caret */
    GrSetGCForeground(gc, COL_CARET);
    GrFillRect(win, gc, 4 + cur_c * CW, 4 + cur_r * CH + CH - 2, CW, 2);
}

static void
draw_cell(int r, int c)
{
    GrSetGCForeground(gc, COL_BG);
    GrFillRect(win, gc, 4 + c * CW, 4 + r * CH, CW, CH);
    GrSetGCForeground(gc, COL_FG);
    GrSetGCBackground(gc, COL_BG);
    char ch = screen[r][c];
    GrText(win, gc, 4 + c * CW, 4 + (r + 1) * CH - 2, &ch, 1, GR_TFASCII);
}

static void put_char(char ch);

static void
erase_eol(void)
{
    for (int c = cur_c; c < COLS; c++) {
        screen[cur_r][c] = ' ';
        draw_cell(cur_r, c);
    }
}

static void
erase_eod(void)
{
    erase_eol();
    for (int r = cur_r + 1; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            screen[r][c] = ' ';
            draw_cell(r, c);
        }
}

static void
do_csi(char final)
{
    int n = esc_p_active_ ? esc_p_[0] : 0;   /* default param */
    int m = esc_n_ > 1 ? esc_p_[1] : 0;
    if (n < 1 && (final == 'A' || final == 'B' || final == 'C' || final == 'D')) n = 1;
    switch (final) {
    case 'A': cur_r -= n; if (cur_r < 0) cur_r = 0; break;     /* up    */
    case 'B': cur_r += n; if (cur_r >= ROWS) cur_r = ROWS - 1; break; /* down  */
    case 'C': cur_c += n; if (cur_c >= COLS) cur_c = COLS - 1; break; /* right */
    case 'D': cur_c -= n; if (cur_c < 0) cur_c = 0; break;     /* left  */
    case 'H': case 'f':                                         /* CUP   */
        cur_r = (n > 0 ? n - 1 : 0);
        cur_c = (m > 0 ? m - 1 : 0);
        if (cur_r >= ROWS) cur_r = ROWS - 1;
        if (cur_c >= COLS) cur_c = COLS - 1;
        break;
    case 'J':                                                  /* ED    */
        if (n == 0) erase_eod();
        else if (n == 2) { for (int r = 0; r < ROWS; r++)
                              for (int c = 0; c < COLS; c++) {
                                  screen[r][c] = ' '; draw_cell(r, c);
                              }
                           cur_r = cur_c = 0; }
        break;
    case 'K': erase_eol(); break;                              /* EL    */
    case 'm': /* SGR: colors etc — ignored, we're monochrome */ break;
    case 'h': case 'l': /* DEC mode set/reset — ignored */ break;
    case 'r': /* scroll region — ignored */ break;
    default: break;
    }
}

static void
handle_byte(unsigned char b)
{
    switch (esc_state_) {
    case ESC_NONE:
        if (b == 0x1b) {
            esc_state_ = ESC_GOT_ESC;
        } else {
            put_char((char)b);
        }
        return;
    case ESC_GOT_ESC:
        if (b == '[') {
            esc_state_ = ESC_CSI;
            esc_n_ = 0;
            esc_p_active_ = 0;
            for (int i = 0; i < 8; i++) esc_p_[i] = 0;
        } else {
            /* ESC X with X != '[' — unsupported, drop */
            esc_state_ = ESC_NONE;
        }
        return;
    case ESC_CSI:
        if (b >= '0' && b <= '9') {
            if (esc_n_ == 0) esc_n_ = 1;
            esc_p_[esc_n_ - 1] = esc_p_[esc_n_ - 1] * 10 + (b - '0');
            esc_p_active_ = 1;
        } else if (b == ';') {
            if (esc_n_ < 8) esc_n_++;
        } else if (b == '?' || b == '>') {
            /* private-mode prefix — keep consuming until final letter */
        } else if (b >= 0x40 && b <= 0x7e) {
            /* final byte */
            do_csi((char)b);
            esc_state_ = ESC_NONE;
        } else {
            /* unexpected byte in CSI — abort the sequence */
            esc_state_ = ESC_NONE;
        }
        return;
    }
}

static void
put_char(char ch)
{
    switch (ch) {
    case '\r':
        cur_c = 0;
        return;
    case '\n':
        cur_c = 0;
        cur_r++;
        if (cur_r >= ROWS) { scroll_up(); cur_r = ROWS - 1; }
        return;
    case '\b':
        if (cur_c > 0) {
            cur_c--;
            /* don't erase — busybox's line editor often follows with
             * \e[K which clears to end of line. If we erase here we
             * blank the still-visible chars. */
        }
        return;
    case '\t':
        do put_char(' '); while (cur_c % 8 != 0);
        return;
    case 0x07: /* BEL */
        return;
    }
    if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7f) return;
    screen[cur_r][cur_c] = ch;
    draw_cell(cur_r, cur_c);
    cur_c++;
    if (cur_c >= COLS) { cur_c = 0; cur_r++; if (cur_r >= ROWS) { scroll_up(); cur_r = ROWS - 1; draw_all(); } }
}

static void
pump_shell_output(void)
{
    char buf[256];
    /* Non-blocking read — drain whatever is ready. */
    ssize_t n;
    int got = 0;
    while ((n = read(sh_fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) handle_byte((unsigned char)buf[i]);
        got = 1;
    }
    if (got) {
        /* Repaint caret (the per-cell draws above leave it stale). */
        GrSetGCForeground(gc, COL_CARET);
        GrFillRect(win, gc, 4 + cur_c * CW, 4 + cur_r * CH + CH - 2, CW, 2);
    }
}

/* Open a Unix98 pty pair. Returns the master fd or -1; on success,
 * *slave_out is the slave fd (already opened) which the child will
 * dup2 onto 0/1/2 and then close. The slave path is /dev/pts/N for
 * some N, which devpts (mounted at /dev/pts) creates on demand. */
static int
open_pty(int *slave_out)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    /* grantpt() normally chowns the slave to the calling user. On a
     * single-user nommu rootfs we don't have a pt_chown helper, but
     * the slave is already owned by root which is who we run as, so
     * grantpt would be a no-op anyway. Skip it; just unlockpt. */
    if (unlockpt(master) < 0) { close(master); return -1; }
    const char *name = ptsname(master);
    if (!name) { close(master); return -1; }
    int slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0) { close(master); return -1; }
    *slave_out = slave;
    return master;
}

static int
start_shell(void)
{
    int slave = -1;
    int master = open_pty(&slave);
    int use_socketpair = 0;
    int sv[2] = { -1, -1 };

    if (master < 0) {
        /* Fallback: socketpair — line-oriented programs work, but
         * isatty() apps (nano, vi, less) won't run. */
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
        use_socketpair = 1;
        master = sv[0];
        slave  = sv[1];
    }

    pid_t pid = vfork();
    if (pid < 0) {
        close(master); if (slave >= 0) close(slave);
        return -1;
    }
    if (pid == 0) {
        /* CHILD — vfork: must call _exit/execve, no return through
         * caller. dup2 is permitted (per-task fd table). */
        if (!use_socketpair) {
            /* Become session leader and acquire the slave as controlling tty
             * so isatty(0) is true and job-control signals work. */
            setsid();
            ioctl(slave, TIOCSCTTY, 0);
        }
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)  close(slave);
        if (master > 2) close(master);
        char *const argv[] = { "sh", "-i", NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }

    if (slave >= 0) close(slave);
    sh_fd  = master;
    sh_pid = pid;

    /* Explicitly configure the pty's line discipline so we don't depend
     * on whatever busybox left as defaults. In particular VERASE must be
     * the same byte we send for Backspace (0x7f) — without this, BS
     * prints as a literal ^H instead of erasing the previous char. We
     * also force cooked mode + ECHO + signal generation so Ctrl-C
     * interrupts properly and Ctrl-D sends EOF. */
    if (!use_socketpair) {
        struct termios t;
        if (tcgetattr(sh_fd, &t) == 0) {
            t.c_cc[VERASE]  = 0x7f;   /* DEL erases prev char (matches what we send) */
            t.c_cc[VINTR]   = 0x03;   /* Ctrl-C → SIGINT */
            t.c_cc[VEOF]    = 0x04;   /* Ctrl-D → EOF */
            t.c_cc[VKILL]   = 0x15;   /* Ctrl-U → kill line */
            t.c_cc[VWERASE] = 0x17;   /* Ctrl-W → erase word */
            t.c_lflag |= (ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ISIG);
            t.c_iflag |= (ICRNL  | BRKINT | IXON);
            t.c_oflag |= (OPOST  | ONLCR);
            tcsetattr(sh_fd, TCSANOW, &t);
        }
    }

    /* Tell the pty kernel the window size so curses programs lay out
     * correctly. Skipped harmlessly for socketpair (ioctl fails). */
    struct winsize ws = { ROWS, COLS, 0, 0 };
    (void)ioctl(sh_fd, TIOCSWINSZ, &ws);

    int flags = fcntl(sh_fd, F_GETFL, 0);
    fcntl(sh_fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

/* SDL/evdev → ASCII for chars our nano-X kbd driver returns via MWKEY.
 * Microwindows uses MWKEY_* which mostly coincides with ASCII for
 * printable keys plus special codes for arrows etc. */
static int
mwkey_to_ascii(unsigned key, unsigned modif)
{
    /* Linux tty default VERASE is DEL (0x7f), not BS (0x08) — sending BS
     * leaves a literal ^H in the line buffer. */
    if (key == MWKEY_BACKSPACE || key == '\b')  return 0x7f;
    /* tty line discipline (ICRNL) maps CR → NL on input, so sending '\r'
     * is what a real terminal does for the Enter key. */
    if (key == MWKEY_ENTER || key == '\r' || key == '\n') return '\r';
    if (key == MWKEY_TAB)                                  return '\t';
    if (key == MWKEY_ESCAPE)                               return 0x1b;
    if (key < 0x20 || key > 0x7e)                          return -1;
    /* Apply shift for letters (the kbd driver reports modifier state
     * separately; we fold it into the byte we send). */
    if (modif & (MWKMOD_LSHIFT | MWKMOD_RSHIFT)) {
        if (key >= 'a' && key <= 'z') return key - 32;
    }
    return key;
}

int
main(void)
{
    /* Tell ncurses-based apps (nano, vi, less, top) what we are so they
     * pick the right terminfo capabilities. vt100 ships in buildroot's
     * default subset and matches our parser well — cursor motion, ED, EL,
     * SGR; no fancy colors. Without TERM set, apps print the raw
     * capability strings (%p1%p2…) as literal text. */
    putenv((char *)"TERM=vt100");
    /* Match the pty's window size so curses knows the grid. The shell
     * also picks this up via TIOCGWINSZ on its tty. */
    char rows[16], cols[16];
    snprintf(rows, sizeof(rows), "LINES=%d", ROWS);
    snprintf(cols, sizeof(cols), "COLUMNS=%d", COLS);
    putenv(rows);
    putenv(cols);

    if (GrOpen() < 0) { fprintf(stderr, "rvemu-term: GrOpen failed\n"); return 1; }
    if (start_shell() < 0) { fprintf(stderr, "rvemu-term: shell spawn failed: %s\n", strerror(errno)); return 1; }

    gc = GrNewGC();
    GrSetGCFont(gc, GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL));

    clear_screen();

    win = GrNewWindowEx(GR_WM_PROPS_NOAUTOMOVE, "Term", GR_ROOT_WINDOW_ID,
        10, 10, WIN_W, WIN_H, COL_BG);
    GrSelectEvents(win, GR_EVENT_MASK_EXPOSURE |
                        GR_EVENT_MASK_KEY_DOWN |
                        GR_EVENT_MASK_CLOSE_REQ);
    GrMapWindow(win);

    /* Poll shell output every 50ms in addition to nano-X events. */
    for (;;) {
        GR_EVENT ev;
        GrGetNextEventTimeout(&ev, 50);
        switch (ev.type) {
        case GR_EVENT_TYPE_TIMEOUT:
            pump_shell_output();
            break;
        case GR_EVENT_TYPE_EXPOSURE:
            draw_all();
            break;
        case GR_EVENT_TYPE_KEY_DOWN: {
            int c = mwkey_to_ascii(ev.keystroke.ch, ev.keystroke.modifiers);
            if (c >= 0 && sh_fd >= 0) {
                unsigned char b = (unsigned char)c;
                /* No local echo — the pty's line discipline echoes for
                 * us. Doing both made every keystroke appear twice
                 * (which is why "rvpkg" looked like garbage until you
                 * lucked into the right rhythm). */
                (void)write(sh_fd, &b, 1);
            }
            break;
        }
        case GR_EVENT_TYPE_CLOSE_REQ:
            if (sh_pid > 0) kill(sh_pid, SIGTERM);
            GrClose();
            return 0;
        }
    }
}
