/* stdio_file.c — minimal FILE layer over a tiny in-RAM filesystem.
 *
 * OPT-IN: link this alongside libc.c when a guest needs fopen/fread/fwrite/
 * fgetc/fgets/fprintf on real streams (the c-testsuite runner does). It is a
 * separate object because some guests bring their own FILE functions
 * (Examples/TinyCC/Programs/stubs.c) and must not collide at link time.
 *
 * Streams: stdin/stdout/stderr are the libc.c sentinels (FILE*)0/1/2 — output
 * to them goes to the UART via vprintf/putchar, input comes from getchar().
 * Anything else is a slot in the static file table: a handful of small
 * fixed-size files, persisting across fclose so write-then-reopen-read works.
 */

#include <stdio.h>
#include <string.h>

#define NFILES 4
#define FCAP   4096

struct _FILE {
    char name[32];
    unsigned char data[FCAP];
    unsigned int size, pos;
    int open, exists;
};

static struct _FILE files[NFILES];

static struct _FILE *as_file(FILE *f)
{
    return ((unsigned int)f > 2u) ? (struct _FILE *)f : NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    struct _FILE *slot = NULL;
    for (int i = 0; i < NFILES; i++)
        if (files[i].exists && !strcmp(files[i].name, path)) { slot = &files[i]; break; }
    if (mode[0] == 'r') {
        if (!slot) return NULL;
    } else {                                    /* "w"/"a": create or truncate */
        if (!slot)
            for (int i = 0; i < NFILES; i++)
                if (!files[i].exists) { slot = &files[i]; break; }
        if (!slot) return NULL;
        strncpy(slot->name, path, sizeof slot->name - 1);
        slot->name[sizeof slot->name - 1] = '\0';
        slot->exists = 1;
        if (mode[0] == 'w') slot->size = 0;
    }
    slot->pos = mode[0] == 'a' ? slot->size : 0;
    slot->open = 1;
    return (FILE *)slot;
}

int fclose(FILE *stream)
{
    struct _FILE *f = as_file(stream);
    if (!f) return -1;
    f->open = 0;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t n, FILE *stream)
{
    struct _FILE *f = as_file(stream);
    if (!f || size == 0) return 0;
    size_t bytes = size * n;
    if (bytes > f->size - f->pos) bytes = f->size - f->pos;
    memcpy(ptr, f->data + f->pos, bytes);
    f->pos += (unsigned int)bytes;
    return bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *stream)
{
    struct _FILE *f = as_file(stream);
    size_t bytes = size * n;
    if (!f) {                                   /* stdout/stderr → UART */
        const char *p = (const char *)ptr;
        for (size_t i = 0; i < bytes; i++) putchar(p[i]);
        return n;
    }
    if (size == 0) return 0;
    if (bytes > FCAP - f->pos) bytes = FCAP - f->pos;
    memcpy(f->data + f->pos, ptr, bytes);
    f->pos += (unsigned int)bytes;
    if (f->pos > f->size) f->size = f->pos;
    return bytes / size;
}

int fgetc(FILE *stream)
{
    struct _FILE *f = as_file(stream);
    if (!f) return getchar();                   /* stdin */
    return f->pos < f->size ? f->data[f->pos++] : EOF;
}

int getc(FILE *stream) { return fgetc(stream); }

char *fgets(char *s, int size, FILE *stream)
{
    int n = 0;
    while (n < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        s[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return NULL;
    s[n] = '\0';
    return s;
}

int fseek(FILE *stream, long offset, int whence)
{
    struct _FILE *f = as_file(stream);
    if (!f) return -1;
    long p = whence == 1 ? (long)f->pos + offset
           : whence == 2 ? (long)f->size + offset : offset;
    if (p < 0) p = 0;
    if (p > (long)f->size) p = (long)f->size;
    f->pos = (unsigned int)p;
    return 0;
}

long ftell(FILE *stream)
{
    struct _FILE *f = as_file(stream);
    return f ? (long)f->pos : -1;
}

void rewind(FILE *stream) { fseek(stream, 0, 0); }

int feof(FILE *stream)
{
    struct _FILE *f = as_file(stream);
    return f ? f->pos >= f->size : 0;
}

int fflush(FILE *stream) { (void)stream; return 0; }

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    struct _FILE *f = as_file(stream);
    if (!f) return vprintf(fmt, ap);            /* stdout/stderr → UART */
    char buf[512];
    int len = vsnprintf(buf, sizeof buf, fmt, ap);
    if (len > (int)sizeof buf - 1) len = (int)sizeof buf - 1;
    return (int)fwrite(buf, 1, (size_t)len, stream);
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}
