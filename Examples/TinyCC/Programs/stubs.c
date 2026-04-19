/*
 * stubs.c — Stub implementations of symbols required by TCC's code
 * but never actually called when using tcc_compile_string() with a
 * simple integer function. All file I/O returns error; 128-bit float
 * ops are unreachable for RV32I (LDOUBLE == DOUBLE).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── String utilities ─────────────────────────────────────────────── */

unsigned long long strtoull(const char *s, char **endptr, int base)
{
    /* Use strtoul for the upper 32 bits — sufficient for TCC's linker flags */
    extern unsigned long strtoul(const char *, char **, int);
    return (unsigned long long)strtoul(s, endptr, base);
}

/* strtold: long double == double on RV32I (LDOUBLE_SIZE=8) */
double strtold(const char *s, char **endptr)
{
    extern double strtod(const char *, char **);
    return strtod(s, endptr);
}

char *strerror(int errnum)
{
    (void)errnum;
    return "error";
}

/* ── File I/O stubs (TCC file-based compile path; we use compile_string) */

int close(int fd)              { (void)fd; return -1; }
int read(int fd, void *buf, unsigned int count)
                               { (void)fd; (void)buf; (void)count; return -1; }
int lseek(int fd, int offset, int whence)
                               { (void)fd; (void)offset; (void)whence; return -1; }

typedef struct _FILE FILE;
extern FILE *stdout;
extern FILE *stderr;
FILE *fopen(const char *path, const char *mode)  { (void)path; (void)mode; return (FILE*)0; }
int   fclose(FILE *f)                            { (void)f; return -1; }
int   fflush(FILE *f)                            { (void)f; return 0; }
FILE *freopen(const char *p, const char *m, FILE *f) { (void)p; (void)m; (void)f; return (FILE*)0; }

int fprintf(FILE *f, const char *fmt, ...)
{
    /* Route TCC error messages to stdout so they appear in the emulator output */
    extern int vprintf(const char *fmt, __builtin_va_list ap);
    __builtin_va_list ap;
    (void)f;
    __builtin_va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int unlink(const char *path) { (void)path; return -1; }

/* ── More missing symbols ─────────────────────────────────────────── */

/* realpath: resolve canonical path — just copy src (no FS) */
char *realpath(const char *path, char *resolved)
{
    extern char *strcpy(char *, const char *);
    if (resolved) { strcpy(resolved, path ? path : ""); return resolved; }
    return (char *)0;
}

/* localtime: used by TCC for __DATE__/__TIME__ macros */
struct tm *localtime(const time_t *timep)
{
    /* Return a zero-initialized static tm struct (epoch: 1970-01-01 00:00:00) */
    static struct tm t;
    extern void *memset(void *, int, unsigned int);
    (void)timep;
    memset(&t, 0, sizeof(t));
    t.tm_year = 70;  /* 1970 */
    t.tm_mday = 1;
    return &t;
}

/* ldexpl: long double ldexp — multiply x by 2^exp (same as double in RV32I) */
double ldexpl(double x, int exp)
{
    /* Simple implementation: multiply by 2^exp */
    if (exp >= 0) {
        while (exp-- > 0) x *= 2.0;
    } else {
        while (exp++ < 0) x *= 0.5;
    }
    return x;
}

/* vfprintf: format and print to FILE* — route to vprintf */
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap)
{
    extern int vprintf(const char *, __builtin_va_list);
    (void)f;
    return vprintf(fmt, ap);
}

/* ── 128-bit float stubs (RV32I LDOUBLE==DOUBLE; these are dead code) ─ */
/* These are referenced by TCC's generic codegen but never called because
   we never compile long-double expressions. Provide 0-returning stubs
   that occupy a single instruction so the symbol is defined.          */

typedef struct { unsigned long lo, hi; } tf128;

tf128 __extendsftf2(float x)       { (void)x; tf128 r={0,0}; return r; }
tf128 __extenddftf2(double x)      { (void)x; tf128 r={0,0}; return r; }
float __trunctfsf2(tf128 x)        { (void)x; return 0.0f; }
double __trunctfdf2(tf128 x)       { (void)x; return 0.0; }
tf128 __floatsitf(int x)           { (void)x; tf128 r={0,0}; return r; }
tf128 __floatunsitf(unsigned x)    { (void)x; tf128 r={0,0}; return r; }
tf128 __floatunditf(unsigned long long x) { (void)x; tf128 r={0,0}; return r; }
tf128 __floatditf(long long x)     { (void)x; tf128 r={0,0}; return r; }
long long  __fixtfdi(tf128 x)      { (void)x; return 0LL; }
unsigned long long __fixunstfdi(tf128 x) { (void)x; return 0ULL; }
int  __fixtfsi(tf128 x)            { (void)x; return 0; }
unsigned __fixunstfsi(tf128 x)     { (void)x; return 0U; }
int  __netf2(tf128 a, tf128 b)     { (void)a; (void)b; return 0; }
int  __eqtf2(tf128 a, tf128 b)     { (void)a; (void)b; return 0; }
int  __lttf2(tf128 a, tf128 b)     { (void)a; (void)b; return -1; }
int  __letf2(tf128 a, tf128 b)     { (void)a; (void)b; return -1; }
int  __gttf2(tf128 a, tf128 b)     { (void)a; (void)b; return 1; }
int  __getf2(tf128 a, tf128 b)     { (void)a; (void)b; return 1; }
tf128 __addtf3(tf128 a, tf128 b)   { (void)a; (void)b; tf128 r={0,0}; return r; }
tf128 __subtf3(tf128 a, tf128 b)   { (void)a; (void)b; tf128 r={0,0}; return r; }
tf128 __multf3(tf128 a, tf128 b)   { (void)a; (void)b; tf128 r={0,0}; return r; }
tf128 __divtf3(tf128 a, tf128 b)   { (void)a; (void)b; tf128 r={0,0}; return r; }
tf128 __negtf2(tf128 a)            { (void)a;           tf128 r={0,0}; return r; }
