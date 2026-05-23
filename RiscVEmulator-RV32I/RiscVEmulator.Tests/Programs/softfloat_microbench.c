/* softfloat_microbench.c — exercise __mulsf3 / __addsf3 in a tight loop
 * via volatile to defeat constant folding, then halt with the iteration
 * count. Used to compare JIT-only vs JIT-with-SSE-shortcut. */
typedef unsigned int u32;
#define UART     (*(volatile unsigned char *)0x10000000)
#define HOSTEXIT (*(volatile unsigned int  *)0x40000000)

static void puts_raw(const char *s) { while (*s) UART = (unsigned char)*s++; }
static void put_int(int v)
{
    char b[12]; int n = 0;
    if (v == 0) { UART = '0'; return; }
    if (v < 0) { UART = '-'; v = -v; }
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n--) UART = b[n];
}

void _start(void)
{
    /* 10 million iterations of (a * b + c) — six softfloat calls per iter
     * (mul + add) since `volatile` defeats fusion. */
    volatile float a = 1.000001f, b = 0.999999f, c = 0.5f;
    float acc = 0.0f;
    for (int i = 0; i < 1000000; i++) {
        acc = acc + a * b + c;
    }
    /* Use acc so it isn't dead-eliminated. */
    u32 bits = *(u32 *)&acc;
    puts_raw("acc.bits=0x");
    for (int i = 7; i >= 0; i--) {
        u32 n = (bits >> (i * 4)) & 0xF;
        UART = (unsigned char)(n < 10 ? '0' + n : 'a' + n - 10);
    }
    UART = '\n';
    HOSTEXIT = 0;
    for (;;) {}
}
