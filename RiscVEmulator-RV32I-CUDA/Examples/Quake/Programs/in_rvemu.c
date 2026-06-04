/* in_rvemu.c — Input driver against the emulator's KeyboardDevice
 * (0x10001000) and MouseDevice (0x10002000).
 *
 * Quake's input model: this code calls Key_Event(key, down) for each
 * keyboard transition, and IN_MouseMove(cmd) collects deltas the engine
 * uses to update viewangles. We poll once per VID_ProcessEvents.
 */

#include "quakedef.h"
#include "client.h"
#include "view.h"

/* Cvars tyrquake's in_sdl.c uses; declared in render.h / view.h / client.c */
extern cvar_t sensitivity;
extern cvar_t m_pitch;
extern cvar_t m_yaw;
extern cvar_t m_forward;
extern cvar_t m_side;
extern cvar_t m_freelook;
extern cvar_t cl_maxpitch;
extern cvar_t cl_minpitch;
extern cvar_t lookstrafe;
extern kbutton_t in_strafe;
extern kbutton_t in_mlook;
extern void V_StopPitchDrift(void);

#define KB_STATUS    (*(volatile unsigned int *)0x10001000)
#define KB_DATA      (*(volatile unsigned int *)0x10001004)
#define MOUSE_STATUS (*(volatile unsigned int *)0x10002000)
#define MOUSE_DX     (*(volatile int          *)0x10002004)
#define MOUSE_DY     (*(volatile int          *)0x10002008)
#define MOUSE_BTN    (*(volatile unsigned int *)0x1000200C)

/* SDL keysym → Quake K_* / ASCII keynum.
 *
 * IMPORTANT: SDL keycodes for printable characters are ASCII LOWERCASE
 * (sym = 0x77 for 'w'), NOT Windows VK codes (where VK_W = 0x57). An
 * earlier version of this function was inherited from the DOOM port
 * which received Win-VK codes, and its `case 0x73..0x7B` branches
 * mapped F1-F12 onto codes that, under SDL, were 's', 'w', 'y' etc.
 * That made W/S/Y silently do nothing in game (and Y in the quit
 * dialog became F10). The fix: SDL sends F-keys as the 0x40000058+
 * range which we handle separately in Frontend/SdlWindow.cs's
 * MapKeysym — so we DON'T need any ASCII-overlapping F-key cases
 * here. Printable ASCII falls through unchanged. */
static int
vk_to_quake(unsigned int vk)
{
    /* Specific scancodes FIRST — many of them collide with printable
     * ASCII (arrows 0x25-0x28 collide with %&'(, mousewheel 0xE0/E1
     * collides with high-bit Latin-1, etc.). MUST be matched before
     * the ASCII fallthrough or `K_LEFTARROW` becomes `%` and the
     * main menu can't navigate. */
    switch (vk) {
        case 0x1B: return K_ESCAPE;
        case 0x0D: return K_ENTER;
        case 0x09: return K_TAB;
        case 0x08: return K_BACKSPACE;
        case 0x25: return K_LEFTARROW;
        case 0x26: return K_UPARROW;
        case 0x27: return K_RIGHTARROW;
        case 0x28: return K_DOWNARROW;
        case 0x10: return K_SHIFT;
        case 0x11: return K_CTRL;
        case 0x12: return K_ALT;
        case 0xE0: return K_MWHEELUP;
        case 0xE1: return K_MWHEELDOWN;
    }

    /* Printable ASCII (digits, lowercase letters, punctuation,
     * including the backquote that toggles the console) flows through
     * as the corresponding Quake keynum. */
    if (vk >= 0x20 && vk < 0x7F) return vk;

    /* Uppercase letters (defensive — shouldn't occur with SDL) get
     * lowercased so bindings match. */
    if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 'a';

    return 0;
}

void IN_Init(void)                 { }
void IN_Shutdown(void)             { }
void IN_Commands(void)             { }
void IN_ClearStates(void)          { }
void IN_ModeChanged(void)          { }
void IN_ActivateMouse(void)        { }
void IN_DeactivateMouse(void)      { }

static float mouse_x, mouse_y;
static unsigned int prev_btn;

/* Pump pending mouse button transitions into Key_Event so bindings like
 * `bind MOUSE1 +attack` work. Called from IN_ProcessKeyboard so it runs
 * once per frame on the same drain pass as keyboard. */
static void
poll_mouse_buttons(void)
{
    unsigned int btn = MOUSE_BTN;
    unsigned int changed = btn ^ prev_btn;
    if (changed & 1) Key_Event(K_MOUSE1, (btn & 1) ? 1 : 0);
    if (changed & 2) Key_Event(K_MOUSE2, (btn & 2) ? 1 : 0);
    if (changed & 4) Key_Event(K_MOUSE3, (btn & 4) ? 1 : 0);
    prev_btn = btn;
}

void
IN_Move(usercmd_t *cmd)
{
    /* Pump mouse deltas. Reading the MMIO registers clears the deltas
     * on the host side, so deltas naturally accumulate per-frame. */
    if (MOUSE_STATUS) {
        mouse_x += (float)MOUSE_DX;
        mouse_y += (float)MOUSE_DY;
    }

    /* Apply sensitivity. */
    float mx = mouse_x * sensitivity.value;
    float my = mouse_y * sensitivity.value;

    /* CRITICAL: Quake's V_CalcRefdef applies an automatic "pitch drift"
     * that gradually springs the view back to horizontal whenever
     * we're not actively looking up/down. Mouse-look ports must call
     * V_StopPitchDrift() on every frame the mouse actually moves, or
     * the view re-centers on every release. Matches tyrquake's
     * in_sdl.c::IN_MouseMove behaviour. */
    if ((mouse_x || mouse_y) && ((in_mlook.state & 1) ^ (int)m_freelook.value))
        V_StopPitchDrift();

    /* Strafe vs yaw. */
    if ((in_strafe.state & 1)
        || (lookstrafe.value && ((in_mlook.state & 1) ^ (int)m_freelook.value))) {
        cmd->sidemove += m_side.value * mx;
    } else {
        cl.viewangles[YAW] -= m_yaw.value * mx;
    }

    /* Free-look pitch or forward/back. */
    if (((in_mlook.state & 1) ^ (int)m_freelook.value) && !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch.value * my;
        if (cl.viewangles[PITCH] > cl_maxpitch.value)
            cl.viewangles[PITCH] = cl_maxpitch.value;
        if (cl.viewangles[PITCH] < cl_minpitch.value)
            cl.viewangles[PITCH] = cl_minpitch.value;
    } else {
        cmd->forwardmove -= m_forward.value * my;
    }

    mouse_x = mouse_y = 0.0f;
}

/* Called once per frame from VID_ProcessEvents in vid_rvemu.c.
 *
 * Drains the KeyboardDevice FIFO completely each pass — capping the loop
 * at N events lets the queue accumulate when the user types fast, which
 * desynchronises Quake's key-state machine (each unmatched up/down pair
 * leaves a key "stuck down"). Same encoding the DOOM port uses:
 *   raw & 0xFF  = Win VK scancode
 *   raw & 0x100 = pressed bit (set on press, clear on release).
 */
void
IN_ProcessKeyboard(void)
{
    while (KB_STATUS & 1u) {
        unsigned int evt = KB_DATA;
        int down = (evt & 0x100u) ? 1 : 0;
        int key  = vk_to_quake(evt & 0xFFu);
        if (key) Key_Event(key, down);
    }
    poll_mouse_buttons();
}
