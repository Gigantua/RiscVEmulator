/* assert.h — Assertion macro for bare-metal RV32I */
#pragma once

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
extern void __assert_fail(const char *expr, const char *file, int line);
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__))
#endif
