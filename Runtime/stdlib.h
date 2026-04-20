/* stdlib.h — General utilities for bare-metal RV32I */
#pragma once

typedef __SIZE_TYPE__ size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     32767

/* ── div types ────────────────────────────────────────────────────── */
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;

/* ── Absolute value ───────────────────────────────────────────────── */
int  abs(int n);
long labs(long n);

/* ── String-to-number conversion ──────────────────────────────────── */
int           atoi(const char *s);
long          atol(const char *s);
long          strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
double        strtod(const char *s, char **endptr);
float         strtof(const char *s, char **endptr);
/* strtold omitted: 128-bit long double not available on bare-metal RV32I */

/* ── Dynamic memory (implemented in malloc.c) ─────────────────────── */
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

/* ── Pseudo-random numbers ────────────────────────────────────────── */
int  rand(void);
void srand(unsigned int seed);

/* ── Sorting and searching ────────────────────────────────────────── */
void  qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* ── Division ─────────────────────────────────────────────────────── */
div_t  div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);

/* ── Program termination ──────────────────────────────────────────── */
_Noreturn void abort(void);
_Noreturn void exit(int status);
