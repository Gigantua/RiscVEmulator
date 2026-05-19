/*
 * rv64-mandelbrot.c — bare-metal RV64GC Mandelbrot, ASCII to UART.
 *
 * Self-contained like rv64-isatest.c: own _start, no libc, no ELF runtime.
 * Compiled `-march=rv64imafdc -mabi=lp64d`, loaded flat at 0x80000000, run
 * in M-mode. Uses double-precision FP throughout so it exercises the D
 * extension hot path of rv64gc_core — a deliberate bug-finder, not a port.
 */
#include <stdint.h>

#define UART 0x10000000UL
#define EXIT 0x40000000UL

static void wc(char c)        { *(volatile uint8_t *)UART = (uint8_t)c; }
static void ws(const char *s) { while (*s) wc(*s++); }

#define W 90
#define H 44
#define MAXIT 64

void test_main(void)
{
    const char *pal = " .,-:;!~=+xX$#@";   /* 15 shades */
    ws("=== RV64GC Mandelbrot (double precision) ===\n");

    for (int py = 0; py < H; py++) {
        double ci = (double)py / (double)H * 2.4 - 1.2;
        for (int px = 0; px < W; px++) {
            double cr = (double)px / (double)W * 3.2 - 2.2;
            double zr = 0.0, zi = 0.0;
            int n = 0;
            while (n < MAXIT) {
                double zr2 = zr * zr, zi2 = zi * zi;
                if (zr2 + zi2 > 4.0) break;
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                n++;
            }
            wc(n >= MAXIT ? '@' : pal[(n * 14) / MAXIT]);
        }
        wc('\n');
    }

    ws("=== DONE ===\n");
    *(volatile uint32_t *)EXIT = 0;        /* halt, exit code 0 */
    for (;;) { }
}

__attribute__((section(".text.start"), naked, used))
void _start(void)
{
    __asm__ volatile("li sp, 0x82000000\n\t tail test_main\n");
}
