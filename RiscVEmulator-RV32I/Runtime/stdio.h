/* stdio.h — Standard I/O for bare-metal RV32I */
#pragma once

typedef __SIZE_TYPE__ size_t;
typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

#ifndef NULL
#define NULL ((void *)0)
#endif
#define EOF  (-1)

/* ── FILE type (opaque — full FILE I/O is not implemented) ────────── */
typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* ── Formatted output ─────────────────────────────────────────────── */
int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vprintf(const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ── Character / string output ────────────────────────────────────── */
int putchar(int c);
int puts(const char *s);
int fputs(const char *s, FILE *stream);
int fputc(int c, FILE *stream);

/* ── Formatted input (stub — returns 0) ───────────────────────────── */
int sscanf(const char *str, const char *fmt, ...);

/* ── File streams (implementations vary per example) ──────────────── */
typedef long fpos_t;
#define BUFSIZ 8192

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t n, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *stream);
int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
void  rewind(FILE *stream);
int   feof(FILE *stream);
int   ferror(FILE *stream);
int   fflush(FILE *stream);
int   fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int   fprintf(FILE *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int   fscanf(FILE *stream, const char *fmt, ...);
int   vfprintf(FILE *stream, const char *fmt, va_list ap);
int   getc(FILE *stream);
int   ungetc(int c, FILE *stream);
int   remove(const char *path);
int   rename(const char *old_, const char *new_);
