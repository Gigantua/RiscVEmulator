/* input_test.c — Tests keyboard and mouse peripherals.
 * Reads pre-injected keyboard events and mouse state, reports via UART.
 */
#include <stddef.h>

#define UART_THR       (*(volatile char *)0x10000000)
#define KBD_STATUS     (*(volatile unsigned int *)0x10001000)
#define KBD_DATA       (*(volatile unsigned int *)0x10001004)
#define KBD_MODIFIER   (*(volatile unsigned int *)0x10001008)
#define MOUSE_STATUS   (*(volatile unsigned int *)0x10002000)
#define MOUSE_DX       (*(volatile unsigned int *)0x10002004)
#define MOUSE_DY       (*(volatile unsigned int *)0x10002008)
#define MOUSE_BUTTONS  (*(volatile unsigned int *)0x1000200C)

static void print_str(const char *s)
{
    while (*s) UART_THR = *s++;
}

static void print_uint(unsigned int n)
{
    char buf[12];
    char *p = buf + 11;
    *p = '\0';
    if (n == 0) { *(--p) = '0'; }
    else { while (n) { *(--p) = '0' + (n % 10); n /= 10; } }
    print_str(p);
}

static void print_int(int n)
{
    if (n < 0) { UART_THR = '-'; n = -n; }
    print_uint((unsigned int)n);
}

static void check(const char *label, int pass)
{
    print_str(label);
    print_str(pass ? ": OK\n" : ": FAIL\n");
}

void _start(void)
{
    print_str("input_test\n");

    /* ── Keyboard ──────────────────────────────────── */
    /* Test expects 3 key events pre-injected:
     *   key 0x1E (A) pressed
     *   key 0x30 (B) pressed
     *   key 0x1E (A) released
     */
    check("kbd_has_data", KBD_STATUS != 0);

    unsigned int k1 = KBD_DATA;  /* should be 0x11E = scancode 0x1E, pressed */
    print_str("key1=");
    print_uint(k1);
    print_str("\n");
    check("key1_A_press", k1 == 0x11E);

    unsigned int k2 = KBD_DATA;  /* 0x130 = scancode 0x30, pressed */
    check("key2_B_press", k2 == 0x130);

    unsigned int k3 = KBD_DATA;  /* 0x01E = scancode 0x1E, released */
    check("key3_A_release", k3 == 0x01E);

    check("kbd_empty", KBD_STATUS == 0);

    /* ── Mouse ──────────────────────────────────────── */
    /* Test expects: dx=+10, dy=-5, left button pressed */
    check("mouse_has_data", MOUSE_STATUS != 0);

    int dx = (int)MOUSE_DX;
    int dy = (int)MOUSE_DY;
    unsigned int btn = MOUSE_BUTTONS;

    print_str("dx=");  print_int(dx);  print_str("\n");
    print_str("dy=");  print_int(dy);  print_str("\n");
    print_str("btn="); print_uint(btn); print_str("\n");

    check("mouse_dx_10", dx == 10);
    check("mouse_dy_neg5", dy == -5);
    check("mouse_left_btn", (btn & 1) != 0);

    /* After reading dx/dy, they should reset to 0 */
    int dx2 = (int)MOUSE_DX;
    check("mouse_dx_reset", dx2 == 0);

    /* Exit */
    *(volatile unsigned int *)0x40000000 = 0;   /* HostExit: write exit code -> halt */
    __builtin_unreachable();
}
