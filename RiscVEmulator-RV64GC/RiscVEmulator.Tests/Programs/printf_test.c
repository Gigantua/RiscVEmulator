/* printf_test.c — Test our minimal libc printf implementation.
 * Links with: libc.o syscalls.o runtime.o
 */
#include "libc.h"

void _start(void)
{
    /* Basic strings */
    printf("hello\n");
    printf("%s world\n", "hello");
    puts("puts test");

    /* Integers */
    printf("%d\n", 42);
    printf("%d\n", -123);
    printf("%u\n", 4000000000u);
    printf("%x\n", 0xDEAD);
    printf("%X\n", 0xBEEF);
    printf("%o\n", 8);

    /* Width and padding */
    printf("[%5d]\n", 42);
    printf("[%-5d]\n", 42);
    printf("[%05d]\n", 42);

    /* Char */
    printf("%c\n", 'A');

    /* Pointer */
    int x = 0;
    printf("%p\n", &x);

    /* Float */
    printf("%f\n", 3.14159);
    printf("%.2f\n", 2.71828);
    printf("%.0f\n", 99.9);
    printf("%f\n", -1.5);
    printf("%.1f\n", 0.0);

    /* Percent literal */
    printf("100%%\n");

    /* Mixed */
    printf("name=%s age=%d pi=%.4f\n", "test", 25, 3.14159);

    /* String precision */
    printf("[%.3s]\n", "abcdef");

    /* NULL string */
    printf("%s\n", (char *)0);

    /* Done */
    printf("PASS\n");

    __asm__ volatile("li a7, 93\nli a0, 0\necall" ::: "a0", "a7");
    __builtin_unreachable();
}
