/* malloc_test.c — Tests heap allocator (malloc, free, calloc, realloc).
 *
 * Exercises allocation, deallocation, reuse, calloc zeroing, and
 * realloc data preservation. Outputs results via UART.
 */

#define UART_THR (*(volatile char *)0x10000000)

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

static void print_hex(unsigned int n)
{
    const char *hex = "0123456789ABCDEF";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[n & 0xF];
        n >>= 4;
    }
    print_str("0x");
    print_str(buf);
}

static void check(const char *label, int pass)
{
    print_str(label);
    print_str(pass ? ": OK\n" : ": FAIL\n");
}

/* External functions from malloc.c */
void *malloc(int size);
void free(void *ptr);
void *calloc(int n, int size);
void *realloc(void *ptr, int new_size);

void _start(void)
{
    print_str("malloc_test\n");

    /* Test 1: basic allocation */
    unsigned char *p1 = (unsigned char *)malloc(64);
    check("malloc_nonnull", p1 != 0);

    /* Write and read back */
    for (int i = 0; i < 64; i++) p1[i] = (unsigned char)(i & 0xFF);
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (p1[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
    }
    check("malloc_rw", ok);

    /* Test 2: second allocation is different */
    unsigned char *p2 = (unsigned char *)malloc(128);
    check("malloc2_nonnull", p2 != 0);
    check("malloc2_different", p2 != p1);

    /* Test 3: free and reuse */
    free(p1);
    unsigned char *p3 = (unsigned char *)malloc(32);
    check("reuse_nonnull", p3 != 0);
    /* p3 might reuse p1's block (first-fit) */
    check("reuse_addr", p3 == p1); /* freed 64-byte block, 32 fits */

    /* Test 4: calloc zeros memory */
    unsigned int *p4 = (unsigned int *)calloc(16, 4); /* 64 bytes */
    check("calloc_nonnull", p4 != 0);
    ok = 1;
    for (int i = 0; i < 16; i++) {
        if (p4[i] != 0) { ok = 0; break; }
    }
    check("calloc_zeroed", ok);

    /* Test 5: realloc preserves data */
    unsigned char *p5 = (unsigned char *)malloc(16);
    for (int i = 0; i < 16; i++) p5[i] = (unsigned char)(0xA0 + i);
    unsigned char *p6 = (unsigned char *)realloc(p5, 256);
    check("realloc_nonnull", p6 != 0);
    ok = 1;
    for (int i = 0; i < 16; i++) {
        if (p6[i] != (unsigned char)(0xA0 + i)) { ok = 0; break; }
    }
    check("realloc_preserved", ok);

    /* Test 6: many small allocations */
    void *ptrs[32];
    ok = 1;
    for (int i = 0; i < 32; i++) {
        ptrs[i] = malloc(64);
        if (!ptrs[i]) { ok = 0; break; }
    }
    check("many_allocs", ok);

    /* Free them all */
    for (int i = 0; i < 32; i++) free(ptrs[i]);

    /* Allocate again — should reuse freed blocks */
    void *p7 = malloc(64);
    check("post_free_alloc", p7 != 0);

    /* Test 7: malloc(0) returns null */
    void *p8 = malloc(0);
    check("malloc_zero", p8 == 0);

    /* Test 8: free(null) is safe */
    free((void *)0);
    check("free_null", 1); /* didn't crash */

    print_str("malloc_test: done\n");

    *(volatile unsigned int *)0x40000000 = 0;   /* HostExit: write exit code -> halt */
}
