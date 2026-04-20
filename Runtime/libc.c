/* libc.c — Minimal C standard library for the RV32I emulator.
 *
 * Provides printf (with %d %u %x %s %c %f %% and width/zero-pad),
 * snprintf, sprintf, puts, putchar, and common string/memory functions.
 *
 * All output flows through _write() from syscalls.c → UART MMIO.
 * Float formatting uses integer-only math (no FPU needed).
 *
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O3 -c
 */

#include "libc.h"

/* Provided by syscalls.c */
extern int _write(int fd, const void *buf, unsigned int count);

/* ═══════════════════════════════════════════════════════════════════
 * I/O PRIMITIVES
 * ═══════════════════════════════════════════════════════════════════ */

int putchar(int c)
{
    char ch = (char)c;
    _write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    while (*s) {
        char c = *s++;
        _write(1, &c, 1);
    }
    char nl = '\n';
    _write(1, &nl, 1);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * STRING FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════ */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * MEMORY FUNCTIONS
 * (memset and memcpy are in runtime.c; we add memcmp and memmove)
 * ═══════════════════════════════════════════════════════════════════ */

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* ═══════════════════════════════════════════════════════════════════
 * UTILITY
 * ═══════════════════════════════════════════════════════════════════ */

int abs(int n)
{
    return n < 0 ? -n : n;
}

int atoi(const char *s)
{
    int neg = 0, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

/* ═══════════════════════════════════════════════════════════════════
 * PRINTF ENGINE
 *
 * Supports: %d %i %u %x %X %o %p %s %c %f %% 
 * Modifiers: width, zero-pad (0), left-align (-), long (l)
 * %f: 6 decimal places by default, precision via %.Nf
 * ═══════════════════════════════════════════════════════════════════ */

/* Internal: emit to either a buffer or stdout */
typedef struct {
    char *buf;      /* NULL = write to stdout */
    size_t pos;
    size_t limit;   /* 0 = unlimited (stdout) */
} _out_t;

static void _emit(_out_t *o, char c)
{
    if (o->buf) {
        if (o->limit == 0 || o->pos < o->limit - 1)
            o->buf[o->pos] = c;
    } else {
        _write(1, &c, 1);
    }
    o->pos++;
}

static void _emit_str(_out_t *o, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        _emit(o, s[i]);
}

static void _emit_pad(_out_t *o, char c, int count)
{
    while (count-- > 0) _emit(o, c);
}

/* Unsigned integer to string, returns length written into buf.
 * buf must be at least 12 bytes for decimal, 11 for hex/oct. */
static int _utoa(unsigned int val, char *buf, int base, int uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[12];
    int len = 0;

    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    while (val) {
        tmp[len++] = digits[val % base];
        val /= base;
    }
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    return len;
}

/* Unsigned long (32-bit on RV32I, same as uint, but for %lu/%lx) */
static int _ultoa(unsigned long val, char *buf, int base, int uppercase)
{
    return _utoa((unsigned int)val, buf, base, uppercase);
}

/* Format a float (double promoted by varargs) using integer-only math.
 * We decompose the float into integer and fractional parts. */
static void _fmt_float(_out_t *o, double val, int prec, int width,
                        int zero_pad, int left_align)
{
    char numbuf[48];
    int nlen = 0;

    if (prec < 0) prec = 6;

    /* Handle sign */
    int neg = 0;
    if (val < 0.0) {
        neg = 1;
        val = -val;
    }

    /* Handle special values (inf, nan) — rough check via huge magnitude */
    /* On softfloat without real inf/nan bits, we just print large numbers */

    /* Integer part */
    unsigned int ipart = (unsigned int)val;
    double frac = val - (double)ipart;

    /* Build fractional digits by multiplying up */
    /* Round: add 0.5 * 10^(-prec) */
    double rounder = 0.5;
    for (int i = 0; i < prec; i++)
        rounder /= 10.0;
    frac += rounder;
    if (frac >= 1.0) {
        ipart += 1;
        frac -= 1.0;
    }

    /* Render integer part */
    if (neg) numbuf[nlen++] = '-';
    char itmp[12];
    int ilen = _utoa(ipart, itmp, 10, 0);
    for (int i = 0; i < ilen; i++) numbuf[nlen++] = itmp[i];

    /* Decimal point and fractional digits */
    if (prec > 0) {
        numbuf[nlen++] = '.';
        for (int i = 0; i < prec; i++) {
            frac *= 10.0;
            int digit = (int)frac;
            if (digit > 9) digit = 9;
            numbuf[nlen++] = '0' + digit;
            frac -= (double)digit;
        }
    }

    /* Padding */
    int pad = width - nlen;
    if (!left_align && pad > 0) _emit_pad(o, zero_pad ? '0' : ' ', pad);
    _emit_str(o, numbuf, nlen);
    if (left_align && pad > 0) _emit_pad(o, ' ', pad);
}

static int _vprintf_core(_out_t *o, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            _emit(o, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int zero_pad = 0, left_align = 0;
        while (*fmt == '0' || *fmt == '-') {
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '-') left_align = 1;
            fmt++;
        }
        if (left_align) zero_pad = 0;

        /* Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } /* skip ll, treat as long */

        /* Conversion */
        char numbuf[20];
        int nlen = 0;
        const char *sval;

        switch (*fmt) {
        case 'd': case 'i': {
            int val = is_long ? (int)va_arg(ap, long) : va_arg(ap, int);
            int neg = 0;
            unsigned int uval;
            if (val < 0) { neg = 1; uval = (unsigned int)(-val); }
            else uval = (unsigned int)val;
            nlen = _utoa(uval, numbuf + 1, 10, 0);
            char *start = numbuf + 1;
            if (neg) { start = numbuf; numbuf[0] = '-'; nlen++; }
            int pad = width - nlen;
            if (!left_align) {
                if (zero_pad && !neg) _emit_pad(o, '0', pad);
                else if (zero_pad && neg) { _emit(o, '-'); start++; nlen--; _emit_pad(o, '0', pad); }
                else _emit_pad(o, ' ', pad);
            }
            _emit_str(o, start, nlen);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case 'u': {
            unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long)
                                       : va_arg(ap, unsigned int);
            nlen = _utoa(val, numbuf, 10, 0);
            int pad = width - nlen;
            if (!left_align) _emit_pad(o, zero_pad ? '0' : ' ', pad);
            _emit_str(o, numbuf, nlen);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case 'x': case 'X': {
            unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long)
                                       : va_arg(ap, unsigned int);
            nlen = _utoa(val, numbuf, 16, *fmt == 'X');
            int pad = width - nlen;
            if (!left_align) _emit_pad(o, zero_pad ? '0' : ' ', pad);
            _emit_str(o, numbuf, nlen);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case 'o': {
            unsigned int val = va_arg(ap, unsigned int);
            nlen = _utoa(val, numbuf, 8, 0);
            int pad = width - nlen;
            if (!left_align) _emit_pad(o, zero_pad ? '0' : ' ', pad);
            _emit_str(o, numbuf, nlen);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case 'p': {
            unsigned int val = (unsigned int)va_arg(ap, void *);
            _emit(o, '0'); _emit(o, 'x');
            nlen = _utoa(val, numbuf, 16, 0);
            _emit_pad(o, '0', 8 - nlen);
            _emit_str(o, numbuf, nlen);
            break;
        }
        case 'f': {
            /* float args are promoted to double by varargs */
            double val = va_arg(ap, double);
            _fmt_float(o, val, prec, width, zero_pad, left_align);
            break;
        }
        case 's': {
            sval = va_arg(ap, const char *);
            if (!sval) sval = "(null)";
            int slen = (int)strlen(sval);
            if (prec >= 0 && prec < slen) slen = prec;
            int pad = width - slen;
            if (!left_align && pad > 0) _emit_pad(o, ' ', pad);
            _emit_str(o, sval, slen);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!left_align && pad > 0) _emit_pad(o, ' ', pad);
            _emit(o, c);
            if (left_align && pad > 0) _emit_pad(o, ' ', pad);
            break;
        }
        case '%':
            _emit(o, '%');
            break;
        case '\0':
            goto done;
        default:
            _emit(o, '%');
            _emit(o, *fmt);
            break;
        }
        fmt++;
    }
done:
    return (int)o->pos;
}

/* ═══════════════════════════════════════════════════════════════════
 * PUBLIC PRINTF FAMILY
 * ═══════════════════════════════════════════════════════════════════ */

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_t o = { NULL, 0, 0 };
    int ret = _vprintf_core(&o, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_t o = { buf, 0, 0 };  /* unlimited — caller must ensure space */
    int ret = _vprintf_core(&o, fmt, ap);
    buf[o.pos] = '\0';
    va_end(ap);
    return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_t o = { buf, 0, size };
    int ret = _vprintf_core(&o, fmt, ap);
    if (buf && size > 0)
        buf[o.pos < size ? o.pos : size - 1] = '\0';
    va_end(ap);
    return ret;
}

int vprintf(const char *fmt, va_list ap)
{
    _out_t o = { NULL, 0, 0 };
    return _vprintf_core(&o, fmt, ap);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    _out_t o = { buf, 0, size };
    int ret = _vprintf_core(&o, fmt, ap);
    if (buf && size > 0)
        buf[o.pos < size ? o.pos : size - 1] = '\0';
    return ret;
}

int fputs(const char *s, FILE *stream)
{
    (void)stream;
    while (*s) {
        char c = *s++;
        _write(1, &c, 1);
    }
    return 0;
}

int fputc(int c, FILE *stream)
{
    (void)stream;
    return putchar(c);
}

int sscanf(const char *str, const char *fmt, ...)
{
    (void)str;
    (void)fmt;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * FILE STREAM STUBS
 * ═══════════════════════════════════════════════════════════════════ */

FILE *stdin  = (FILE *)0;
FILE *stdout = (FILE *)1;
FILE *stderr = (FILE *)2;

/* ═══════════════════════════════════════════════════════════════════
 * ADDITIONAL STRING FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════ */

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    while (n--) {
        if (*p == uc) return (void *)p;
        p++;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * ADDITIONAL STDLIB FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════ */

extern void _exit(int status);

long labs(long n)
{
    return n < 0 ? -n : n;
}

long atol(const char *s)
{
    long neg = 0, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

long strtol(const char *s, char **endptr, int base)
{
    const char *start;
    long val = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'
           || *s == '\f' || *s == '\v')
        s++;

    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }

    start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)(s == start ? (s - (neg ? 1 : 0) - ((base == 16 && s > start) ? 0 : 0)) : s);
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *s, char **endptr, int base)
{
    const char *start;
    unsigned long val = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'
           || *s == '\f' || *s == '\v')
        s++;

    if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }

    start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return val;
}

/* strtod/strtof: minimal implementation for bare-metal.
 * Parses the integer part only; sufficient for TCC's float-constant parsing
 * when only integer-valued constants appear.
 * Note: strtold is omitted — 128-bit long double (__floatsitf) is unavailable
 * on bare-metal RV32I.  Use strtod if you need extended range. */
double strtod(const char *s, char **endptr)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (endptr) *endptr = end;
    return (double)n;
}

float strtof(const char *s, char **endptr)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (endptr) *endptr = end;
    return (float)n;
}

static unsigned int _rand_seed = 1;

int rand(void)
{
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (_rand_seed >> 16) & 32767;
}

void srand(unsigned int seed)
{
    _rand_seed = seed;
}

void qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        /* Save element i */
        char *cur = arr + i * size;
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, cur) > 0) {
            j--;
        }
        if (j != i) {
            /* Rotate: move arr[j..i-1] right by one slot, put saved into j */
            /* We need a temp copy of element i */
            char tmp[256];
            char *tbuf = tmp;
            /* Copy element i to temp */
            for (size_t k = 0; k < size; k++) tbuf[k] = cur[k];
            /* Shift elements j..i-1 right by one position */
            memmove(arr + (j + 1) * size, arr + j * size, (i - j) * size);
            /* Place saved element at j */
            for (size_t k = 0; k < size; k++) (arr + j * size)[k] = tbuf[k];
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (void *)(arr + mid * size);
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

div_t div(int numer, int denom)
{
    div_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

void abort(void)
{
    _exit(134);
    __builtin_unreachable();
}

void exit(int status)
{
    _exit(status);
    __builtin_unreachable();
}

/* ═══════════════════════════════════════════════════════════════════
 * ERRNO
 * ═══════════════════════════════════════════════════════════════════ */

int errno = 0;

/* ═══════════════════════════════════════════════════════════════════
 * ASSERT
 * ═══════════════════════════════════════════════════════════════════ */

void __assert_fail(const char *expr, const char *file, int line)
{
    printf("Assertion failed: %s at %s:%d\n", expr, file, line);
    abort();
}

/* ═══════════════════════════════════════════════════════════════════
 * TIME (minimal — reads CLINT mtime)
 * ═══════════════════════════════════════════════════════════════════ */

time_t time(time_t *t)
{
    volatile unsigned int *timer_lo = (volatile unsigned int *)0x0200BFF8;
    volatile unsigned int *timer_hi = (volatile unsigned int *)0x0200BFFC;
    unsigned int lo = *timer_lo;
    unsigned int hi = *timer_hi;
    unsigned long long us = ((unsigned long long)hi << 32) | lo;
    time_t secs = (time_t)(us / 1000000ULL);
    if (t) *t = secs;
    return secs;
}

clock_t clock(void)
{
    volatile unsigned int *timer_lo = (volatile unsigned int *)0x0200BFF8;
    return (clock_t)*timer_lo;
}
