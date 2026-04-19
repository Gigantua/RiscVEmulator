/* syscalls_test.c — Tests newlib-compatible syscall stubs.
 *
 * Exercises _write, _read, _isatty, _fstat, _close, _lseek,
 * _getpid, _kill (indirectly), _sbrk, _gettimeofday, and _exit.
 * Uses syscalls.c stubs (NOT inline asm) for all system operations.
 */

/* Declare syscall functions from syscalls.c */
int _write(int fd, const void *buf, unsigned int count);
int _read(int fd, void *buf, unsigned int count);
int _open(const char *path, int flags, int mode);
int _close(int fd);
int _lseek(int fd, int offset, int whence);
int _isatty(int fd);
int _getpid(void);
void *_sbrk(int incr);
void _exit(int status);

struct timeval { int tv_sec; int tv_usec; };
int _gettimeofday(struct timeval *tv, void *tz);

/* Minimal fstat struct matching syscalls.c */
struct _minimal_stat {
    unsigned int _pad[3];
    unsigned int st_mode;
};
int _fstat(int fd, struct _minimal_stat *st);

#define S_IFCHR 0020000

/* Helpers — use _write for output instead of direct UART MMIO */
static void print_str(const char *s)
{
    int len = 0;
    const char *p = s;
    while (*p++) len++;
    _write(1, s, len);
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

void _start(void)
{
    print_str("syscalls_test\n");

    /* Test 1: _write to stdout returns count */
    {
        const char msg[] = "hello";
        int ret = _write(1, msg, 5);
        /* "hello" should appear in output, and ret should be 5 */
        print_str("\n"); /* newline after "hello" */
        check("write_stdout_ret", ret == 5);
    }

    /* Test 2: _write to stderr also works */
    {
        const char msg[] = "err";
        int ret = _write(2, msg, 3);
        print_str("\n");
        check("write_stderr_ret", ret == 3);
    }

    /* Test 3: _write to invalid fd returns -1 */
    {
        const char msg[] = "x";
        int ret = _write(99, msg, 1);
        check("write_badfd", ret == -1);
    }

    /* Test 4: _read returns 0 (EOF, no stdin) */
    {
        char buf[8];
        int ret = _read(0, buf, 8);
        check("read_eof", ret == 0);
    }

    /* Test 5: _open always returns -1 (no filesystem) */
    {
        int ret = _open("file.txt", 0, 0);
        check("open_fail", ret == -1);
    }

    /* Test 6: _close returns -1 */
    {
        int ret = _close(3);
        check("close_stub", ret == -1);
    }

    /* Test 7: _lseek returns 0 */
    {
        int ret = _lseek(1, 0, 0);
        check("lseek_zero", ret == 0);
    }

    /* Test 8: _isatty returns 1 */
    {
        check("isatty_stdout", _isatty(1) == 1);
        check("isatty_stdin", _isatty(0) == 1);
    }

    /* Test 9: _fstat sets st_mode to S_IFCHR */
    {
        struct _minimal_stat st;
        st.st_mode = 0;
        int ret = _fstat(1, &st);
        check("fstat_ret", ret == 0);
        check("fstat_mode", st.st_mode == S_IFCHR);
    }

    /* Test 10: _getpid returns 1 */
    {
        check("getpid_1", _getpid() == 1);
    }

    /* Test 11: _sbrk returns incrementing pointers */
    {
        void *p1 = _sbrk(0);   /* current break */
        check("sbrk_nonnull", p1 != (void *)0);

        void *p2 = _sbrk(64);  /* allocate 64 bytes */
        check("sbrk_returns_prev", p2 == p1);

        void *p3 = _sbrk(0);   /* new break should be 64 bytes later */
        check("sbrk_advanced", (char *)p3 == (char *)p1 + 64);

        void *p4 = _sbrk(128); /* allocate more */
        check("sbrk_second", p4 == p3);
        check("sbrk_total", (char *)_sbrk(0) == (char *)p1 + 64 + 128);
    }

    /* Test 12: _gettimeofday returns something (timer running) */
    {
        struct timeval tv;
        tv.tv_sec = -1;
        tv.tv_usec = -1;
        int ret = _gettimeofday(&tv, (void *)0);
        check("gettimeofday_ret", ret == 0);
        check("gettimeofday_sec", tv.tv_sec >= 0);
        check("gettimeofday_usec", tv.tv_usec >= 0);

        /* Second call should show time has advanced */
        struct timeval tv2;
        _gettimeofday(&tv2, (void *)0);
        unsigned long long t1 = (unsigned long long)tv.tv_sec * 1000000 + tv.tv_usec;
        unsigned long long t2 = (unsigned long long)tv2.tv_sec * 1000000 + tv2.tv_usec;
        check("gettimeofday_advances", t2 > t1);
    }

    /* Test 13: _write with count=0 */
    {
        int ret = _write(1, "x", 0);
        check("write_zero", ret == 0);
    }

    print_str("syscalls_test: done\n");

    /* Use _exit() function instead of inline asm */
    _exit(0);
}
