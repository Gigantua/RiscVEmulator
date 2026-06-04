/* quake_main.c — bare-metal _start that invokes Quake's Host_Init /
 * Host_Frame loop. Glue between our linker.ld _start convention and
 * TyrQuake's NQ entry point.
 */

#include "quakedef.h"
#include "common.h"
#include "host.h"
#include "keys.h"
#include "cmd.h"
#include "cvar.h"
#include "libc.h"

extern void IN_ProcessKeyboard(void);
extern void Sys_Printf(const char *fmt, ...);

#define UART_THR  (*(volatile unsigned char *)0x10000000)
#define HOST_EXIT (*(volatile unsigned int  *)0x40000000)
#define RTC_US_LO (*(volatile unsigned int  *)0x10003000)
#define RTC_US_HI (*(volatile unsigned int  *)0x10003004)

static double
now_seconds(void)
{
    unsigned int lo = RTC_US_LO;
    unsigned int hi = RTC_US_HI;
    unsigned long long us = ((unsigned long long)hi << 32) | lo;
    return (double)us * (1.0 / 1000000.0);
}

/* Linker entry. */
void
_start(void)
{
    /* TyrQuake hunk memory is allocated up-front. Keep it modest so
     * malloc can still hand stuff out elsewhere. */
    static quakeparms_t parms;
    /* 32 MiB hunk: the shareware path needs >16 MiB once we leave the
     * demo loop and load `start.bsp` (sound+model caches push past the
     * old 16 MiB ceiling and trip Hunk_AllocName_Raw). */
    static char hunk_storage[32 * 1024 * 1024];
    /* Default is +map autostart — drops the player directly into e1m1
     * so WASD/mouse-look work from frame 1 without navigating the menu.
     * Define RVEMU_QUAKE_MENU to skip autostart and land in the main
     * menu with demos playing behind it (vanilla startup). */
    static const char *fake_argv_menu[]  = { "quake", "-basedir", "/", 0 };
    static const char *fake_argv_warp[]  = { "quake", "-basedir", "/", "+skill", "1", "+map", "e1m1", 0 };
    int automap = 1;
#ifdef RVEMU_QUAKE_MENU
    automap = 0;
#endif
    const char **fake_argv = automap ? fake_argv_warp : fake_argv_menu;

    parms.argc      = automap ? 7 : 3;
    parms.argv      = fake_argv;
    parms.basedir   = "/";
    parms.memsize   = sizeof(hunk_storage);
    parms.membase   = hunk_storage;

    Sys_Printf("[trace] _start: argc=%d\n", parms.argc);
    COM_InitArgv(parms.argc, parms.argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    Sys_Printf("[trace] calling Host_Init...\n");
    Host_Init(&parms, NULL);
    Sys_Printf("[trace] Host_Init returned, entering frame loop\n");

    /* Bind directly via Key_SetBinding — bypasses Cbuf entirely, which
     * sometimes drops bindings between exec'd .cfg files in obscure
     * ways. pak0.pak's default.cfg has already set vintage 1996 bindings
     * (a=+lookup, d=+moveup, arrows for movement); these overwrite. */
    Key_SetBinding((knum_t)'w', "+forward");
    Key_SetBinding((knum_t)'s', "+back");
    Key_SetBinding((knum_t)'a', "+moveleft");
    Key_SetBinding((knum_t)'d', "+moveright");
    Key_SetBinding(K_UPARROW,    "+forward");
    Key_SetBinding(K_DOWNARROW,  "+back");
    Key_SetBinding(K_LEFTARROW,  "+moveleft");
    Key_SetBinding(K_RIGHTARROW, "+moveright");
    Key_SetBinding(K_SPACE,      "+jump");
    Key_SetBinding(K_ENTER,      "+jump");
    Key_SetBinding(K_CTRL,       "+attack");
    Key_SetBinding(K_SHIFT,      "+speed");
    Key_SetBinding(K_TAB,        "+showscores");
    Key_SetBinding(K_MOUSE1,     "+attack");
    Key_SetBinding(K_MOUSE2,     "+forward");
    Key_SetBinding(K_MWHEELUP,   "impulse 10");   /* next weapon */
    Key_SetBinding(K_MWHEELDOWN, "impulse 12");   /* prev weapon */
    Key_SetBinding(K_BACKQUOTE,  "toggleconsole");
    /* Weapon hotkeys 1..8 */
    Key_SetBinding((knum_t)'1', "impulse 1");
    Key_SetBinding((knum_t)'2', "impulse 2");
    Key_SetBinding((knum_t)'3', "impulse 3");
    Key_SetBinding((knum_t)'4', "impulse 4");
    Key_SetBinding((knum_t)'5', "impulse 5");
    Key_SetBinding((knum_t)'6', "impulse 6");
    Key_SetBinding((knum_t)'7', "impulse 7");
    Key_SetBinding((knum_t)'8', "impulse 8");

    /* Cvars: kill the pitch-spring residue, enable free-look, vanilla sens. */
    Cvar_Set("lookspring",  "0");
    Cvar_Set("lookstrafe",  "0");
    Cvar_Set("m_freelook",  "1");
    Cvar_Set("m_pitch",     "0.022");
    Cvar_Set("m_yaw",       "0.022");
    Cvar_Set("sensitivity", "3");

    /* Sanity-print the final state. If your console shows
     * `[rvemu] w bound: +forward` for w/s but not for a/d, that
     * proves the binding step worked and the bug is downstream. */
    Sys_Printf("[rvemu] w bound: %s\n", keybindings[(unsigned char)'w'] ? keybindings[(unsigned char)'w'] : "<NULL>");
    Sys_Printf("[rvemu] s bound: %s\n", keybindings[(unsigned char)'s'] ? keybindings[(unsigned char)'s'] : "<NULL>");
    Sys_Printf("[rvemu] a bound: %s\n", keybindings[(unsigned char)'a'] ? keybindings[(unsigned char)'a'] : "<NULL>");
    Sys_Printf("[rvemu] d bound: %s\n", keybindings[(unsigned char)'d'] ? keybindings[(unsigned char)'d'] : "<NULL>");

    /* One-shot pin so the very first frame's SV_Physics gate sees
     * key_game and progresses signon. After this the engine owns
     * key_dest — pressing ESC opens the menu and it stays open. */
    if (automap) key_dest = key_game;

    /* No outer-loop RTC spin: now_seconds() does two MMIO reads per call
     * which trigger VEH faults — at 1000+ fps that's millions of faults
     * per second of pure overhead. Host_FilterTime inside Host_Frame
     * already throttles via the host_framerate / sys_ticrate cvars, so
     * we let it own the timing entirely and just feed it a fresh dt
     * once per pass. */
    double old = now_seconds();
    unsigned long frame_n = 0;
    for (;;) {
        double t  = now_seconds();
        double dt = t - old;
        old = t;
        IN_ProcessKeyboard();
        /* Heartbeat every 600 frames — at 60 fps that's once every 10s,
         * at 1000 fps once a second. Quiet enough not to drag the UART. */
        if ((frame_n % 600) == 0)
            Sys_Printf("[trace] frame=%lu cls.state=%d\n", frame_n, (int)cls.state);
        Host_Frame((float)dt);
        frame_n++;
    }
}
