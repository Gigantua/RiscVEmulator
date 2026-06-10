/* test_syscalls.c — syscall layer + _start for the c-testsuite runner.
 *
 * Replaces Runtime/syscalls.c AND Runtime/crt0.c for test guests. The shipped
 * map puts UART/host-exit at 0x10000000/0x40000000 — a ~1 GiB flat buffer per
 * core. To run one test per CUDA core (hundreds of cores) each guest instead
 * gets a small self-contained RAM with an IO block the host reads after halt:
 *
 *   0x001E0000  u32 done        0 while running, 1 once _exit ran
 *   0x001E0004  u32 exit code   main's return value
 *   0x001E0008  u32 stdout len  bytes written (may exceed the capture cap)
 *   0x001E0010  stdout bytes    capped at 0xFF00
 *
 * _exit halts the core with ebreak so it stops burning kernel budget.
 * Must match the host-side constants in Examples/CudaTests/Program.cs.
 */

typedef unsigned int u32;

#define IO_DONE (*(volatile u32 *)0x001E0000)
#define IO_CODE (*(volatile u32 *)0x001E0004)
#define IO_LEN  (*(volatile u32 *)0x001E0008)
#define IO_BUF  ((volatile unsigned char *)0x001E0010)
#define IO_CAP  0xFF00u

extern int main(int argc, char **argv);

void _exit(int status)
{
    IO_CODE = (u32)status;
    IO_DONE = 1;
    for (;;) __asm__ volatile ("ebreak");
}

void _start(void)
{
    static char *argv[] = { "test", 0 };
    _exit(main(1, argv));
}

int _write(int fd, const void *buf, unsigned int count)
{
    if (fd != 1 && fd != 2) return -1;
    const unsigned char *p = (const unsigned char *)buf;
    u32 len = IO_LEN;
    for (unsigned int i = 0; i < count; i++, len++)
        if (len < IO_CAP) IO_BUF[len] = p[i];
    IO_LEN = len;
    return (int)count;
}

int _read(int fd, void *buf, unsigned int count)
{
    (void)fd; (void)buf; (void)count;
    return 0;                       /* no stdin in test mode — EOF */
}

int  _getpid(void) { return 1; }
int  _kill(int pid, int sig) { if (pid == 1) _exit(128 + sig); return -1; }
int  _open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int  _close(int fd)  { (void)fd; return -1; }
int  _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int  _isatty(int fd) { (void)fd; return 1; }

struct _minimal_stat { unsigned int _pad[3]; unsigned int st_mode; };
int _fstat(int fd, struct _minimal_stat *st)
{
    (void)fd;
    unsigned char *p = (unsigned char *)st;
    for (int i = 0; i < (int)sizeof(struct _minimal_stat); i++) p[i] = 0;
    st->st_mode = 0020000;          /* S_IFCHR */
    return 0;
}

extern char _heap_start;            /* from linker.ld */
static char *_sbrk_ptr = 0;
void *_sbrk(int incr)
{
    if (!_sbrk_ptr) _sbrk_ptr = &_heap_start;
    char *prev = _sbrk_ptr;
    _sbrk_ptr += incr;
    return (void *)prev;
}

struct timeval { int tv_sec; int tv_usec; };
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }    /* no RTC page in test RAM */
    return 0;
}

static char *_env_null = 0;
char **environ = &_env_null;
