/* syscalls.c — Newlib-compatible syscall stubs for bare-metal RV32I.
 *
 * These implement the minimal set of POSIX-like functions that newlib
 * (or any freestanding C program) needs to link against. They translate
 * to our emulator's ecall ABI and MMIO peripherals.
 *
 * Programs can call _write(), _exit(), etc. instead of using inline asm.
 * This also enables future use of newlib's printf/scanf if desired.
 *
 * Emulator ecall ABI (Linux-style):
 *   a7 = syscall number, a0-a2 = arguments, a0 = return value
 *   93/94 = exit/exit_group
 *   64    = write(fd, buf, len)
 *   63    = read(fd, buf, len)
 *
 * MMIO:
 *   0x10000000 = UART THR (write byte)
 *   0x0200BFF8 = CLINT mtime low  (read: instruction count)
 *   0x0200BFFC = CLINT mtime high
 *
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O3 -c
 */

typedef unsigned int u32;

/* ═══════════════════════════════════════════════════════════════════
 * PROCESS CONTROL
 * ═══════════════════════════════════════════════════════════════════ */

/* _exit: terminate the program by writing the exit code to the host-exit
 * peripheral at 0x40000000. The guarded MMIO write is intercepted by the
 * host's page-fault dispatcher, which sets the exit code and halts the CPU. */
void _exit(int status)
{
    volatile unsigned int *exit_reg = (volatile unsigned int *)0x40000000;
    *exit_reg = (unsigned int)status;
    /* Page-fault handler halts the CPU, so this loop never actually iterates. */
    for (;;);
}

/* _getpid: return process ID. Always 1 on bare metal. */
int _getpid(void)
{
    return 1;
}

/* _kill: send signal to process. On bare metal, only handle self-kill. */
int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    /* If killing ourselves (abort), just exit */
    if (pid == 1) _exit(128 + sig);
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * I/O — FILE DESCRIPTOR OPERATIONS
 * ═══════════════════════════════════════════════════════════════════ */

/* _write: send bytes to fd. fd=1 (stdout) and fd=2 (stderr) go to UART THR.
 * Each byte write is a guarded MMIO access — the host VEH catches the AV
 * and routes through UartDevice.Write → OutputHandler. No ecall needed. */
int _write(int fd, const void *buf, unsigned int count)
{
    if (fd == 1 || fd == 2) {
        volatile unsigned char *uart_thr = (volatile unsigned char *)0x10000000;
        const unsigned char *p = (const unsigned char *)buf;
        for (unsigned int i = 0; i < count; i++)
            *uart_thr = p[i];
        return (int)count;
    }
    return -1;
}

/* _read: receive bytes from fd. fd=0 (stdin) reads from UART (stubbed). */
int _read(int fd, void *buf, unsigned int count)
{
    (void)fd;
    (void)buf;
    (void)count;
    /* No stdin on bare metal — return 0 (EOF) */
    return 0;
}

/* _open: open a file. No filesystem — always fails. */
int _open(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return -1;
}

/* _close: close a file descriptor. Stubs for stdin/stdout/stderr. */
int _close(int fd)
{
    (void)fd;
    return -1;
}

/* _lseek: reposition file offset. Not seekable on bare metal. */
int _lseek(int fd, int offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    return 0;
}

/* Minimal stat structure — only st_mode matters for newlib. */
struct _minimal_stat {
    unsigned int _pad[3];  /* st_dev, st_ino, st_nlink */
    unsigned int st_mode;
    /* rest doesn't matter for our purposes */
};
#define S_IFCHR 0020000 /* character device */

/* _fstat: get file status. Reports all fds as character devices
 * so stdout is line-buffered (not block-buffered). */
int _fstat(int fd, struct _minimal_stat *st)
{
    (void)fd;
    /* Zero everything, then set character device mode */
    unsigned char *p = (unsigned char *)st;
    for (int i = 0; i < (int)sizeof(struct _minimal_stat); i++) p[i] = 0;
    st->st_mode = S_IFCHR;
    return 0;
}

/* _isatty: return 1 to indicate fd is a terminal (forces line buffering). */
int _isatty(int fd)
{
    (void)fd;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * MEMORY / HEAP
 * ═══════════════════════════════════════════════════════════════════ */

/* _sbrk: grow the heap. Used by newlib's malloc.
 * We already have our own malloc in malloc.c, but this is needed
 * if anyone uses newlib's malloc or if printf mallocs internally. */
extern char _heap_start; /* from linker.ld */

static char *_sbrk_ptr = 0;

void *_sbrk(int incr)
{
    if (!_sbrk_ptr) _sbrk_ptr = &_heap_start;
    char *prev = _sbrk_ptr;
    _sbrk_ptr += incr;
    return (void *)prev;
}

/* ═══════════════════════════════════════════════════════════════════
 * TIME
 * ═══════════════════════════════════════════════════════════════════ */

struct timeval {
    int tv_sec;
    int tv_usec;
};

/* _gettimeofday: read wall-clock time from Real-Time Clock peripheral.
 * Falls back to CLINT tick counter if RTC reads zero (headless/test mode). */
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        /* Try RTC first (real wall-clock microseconds) */
        volatile unsigned int *rtc_us_lo = (volatile unsigned int *)0x10003000;
        volatile unsigned int *rtc_us_hi = (volatile unsigned int *)0x10003004;
        unsigned int lo = *rtc_us_lo;
        unsigned int hi = *rtc_us_hi;
        unsigned long long us = ((unsigned long long)hi << 32) | lo;

        if (us == 0) {
            /* Fallback: CLINT timer (instruction-count, ~1µs per tick) */
            volatile unsigned int *timer_lo = (volatile unsigned int *)0x0200BFF8;
            volatile unsigned int *timer_hi = (volatile unsigned int *)0x0200BFFC;
            lo = *timer_lo;
            hi = *timer_hi;
            us = ((unsigned long long)hi << 32) | lo;
        }

        tv->tv_sec  = (int)(us / 1000000ULL);
        tv->tv_usec = (int)(us % 1000000ULL);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * ENVIRONMENT
 * ═══════════════════════════════════════════════════════════════════ */

/* environ: required global. Empty environment on bare metal. */
static char *_env_null = 0;
char **environ = &_env_null;
