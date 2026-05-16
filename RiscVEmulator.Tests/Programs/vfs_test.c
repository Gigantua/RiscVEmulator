/* vfs_test.c — Tests in-memory virtual file system.
 *
 * Registers a small buffer as a "file", then exercises open/read/seek/tell/eof.
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

static void check(const char *label, int pass)
{
    print_str(label);
    print_str(pass ? ": OK\n" : ": FAIL\n");
}

/* VFS functions from vfs.c */
void vfs_register(const char *name, void *data, unsigned int size);
void *vfs_open(const char *filename, const char *mode);
void vfs_close(void *handle);
int vfs_read(void *handle, void *buf, int count);
int vfs_write(void *handle, const void *buf, int count);
int vfs_seek(void *handle, int offset, unsigned int origin);
int vfs_tell(void *handle);
int vfs_eof(void *handle);

/* Test data */
static const unsigned char test_data[] = {
    'H', 'e', 'l', 'l', 'o', ',', ' ',
    'W', 'o', 'r', 'l', 'd', '!', '\n',
    0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34
};
#define TEST_SIZE 20

void _start(void)
{
    print_str("vfs_test\n");

    /* Register a test file */
    vfs_register("test.dat", (void *)test_data, TEST_SIZE);

    /* Test 1: open */
    void *f = vfs_open("test.dat", "rb");
    check("open_ok", f != (void *)0);

    /* Test 2: tell at start */
    int pos = vfs_tell(f);
    check("tell_start", pos == 0);

    /* Test 3: eof at start */
    check("eof_start_no", vfs_eof(f) == 0);

    /* Test 4: read first 7 bytes "Hello, " */
    unsigned char buf[32];
    int n = vfs_read(f, buf, 7);
    check("read_count_7", n == 7);
    check("read_H", buf[0] == 'H');
    check("read_e", buf[1] == 'e');
    check("read_comma_space", buf[5] == ',' && buf[6] == ' ');

    /* Test 5: tell after read */
    pos = vfs_tell(f);
    check("tell_7", pos == 7);

    /* Test 6: read next 7 bytes "World!\n" */
    n = vfs_read(f, buf, 7);
    check("read2_count", n == 7);
    check("read2_W", buf[0] == 'W');
    check("read2_newline", buf[6] == '\n');

    /* Test 7: tell at 14 */
    pos = vfs_tell(f);
    check("tell_14", pos == 14);

    /* Test 8: read remaining 6 bytes */
    n = vfs_read(f, buf, 100); /* ask for more than available */
    check("read3_count", n == 6);
    check("read3_DE", buf[0] == 0xDE);
    check("read3_EF", buf[2] == 0xBE);

    /* Test 9: now at eof */
    check("eof_yes", vfs_eof(f) == 1);

    /* Test 10: read at eof returns 0 */
    n = vfs_read(f, buf, 10);
    check("read_eof_0", n == 0);

    /* Test 11: seek to start (SEEK_SET=0) */
    int ret = vfs_seek(f, 0, 0);
    check("seek_set_ok", ret == 0);
    check("seek_set_pos", vfs_tell(f) == 0);
    check("seek_set_noeof", vfs_eof(f) == 0);

    /* Test 12: re-read first byte */
    n = vfs_read(f, buf, 1);
    check("reread_H", n == 1 && buf[0] == 'H');

    /* Test 13: seek relative (SEEK_CUR=1) */
    vfs_seek(f, 6, 1); /* skip 6 from pos 1 → pos 7 */
    check("seek_cur_pos", vfs_tell(f) == 7);
    n = vfs_read(f, buf, 1);
    check("seek_cur_W", buf[0] == 'W');

    /* Test 14: seek from end (SEEK_END=2) */
    vfs_seek(f, -6, 2); /* 6 from end → pos 14 */
    check("seek_end_pos", vfs_tell(f) == 14);
    n = vfs_read(f, buf, 1);
    check("seek_end_DE", buf[0] == 0xDE);

    /* Test 15: open nonexistent file */
    void *f2 = vfs_open("nofile.dat", "rb");
    check("open_noexist_null", f2 == (void *)0);

    /* Test 16: close and verify */
    vfs_close(f);
    check("close_ok", 1); /* didn't crash */

    /* Test 17: open by path with directory prefix */
    void *f3 = vfs_open("/some/path/test.dat", "rb");
    check("open_by_basename", f3 != (void *)0);
    if (f3) {
        n = vfs_read(f3, buf, 5);
        check("basename_read", n == 5 && buf[0] == 'H');
        vfs_close(f3);
    }

    print_str("vfs_test: done\n");

    *(volatile unsigned int *)0x40000000 = 0;   /* HostExit: write exit code -> halt */
}
