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
