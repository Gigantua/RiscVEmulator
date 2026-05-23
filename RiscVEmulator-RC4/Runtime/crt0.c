/* crt0.c — Minimal C runtime startup for bare-metal RV32I.
 *
 * Provides _start which calls main() and exits.
 * Include this file instead of (or alongside) syscalls.c when the
 * test/application defines main() rather than _start().
 *
 * Programs that define their own _start() will shadow this one at link time.
 */

extern int  main(void);
extern void _exit(int status);

void _start(void)
{
    _exit(main());
}
