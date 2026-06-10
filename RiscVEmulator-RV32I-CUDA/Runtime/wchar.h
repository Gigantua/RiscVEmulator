/* wchar.h — minimal: just enough for guests that iterate L"" literals.
 * wchar_t on riscv32 is a 32-bit int; the compiler encodes L"" as UTF-32. */
#pragma once

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif
typedef __WINT_TYPE__ wint_t;

#define WEOF ((wint_t)-1)
