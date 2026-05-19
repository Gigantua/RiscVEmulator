#pragma once

/* Exact-width integer types for RV32I */
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

/* Minimum-width */
typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

/* Fast */
typedef int8_t   int_fast8_t;
typedef int32_t  int_fast16_t;
typedef int32_t  int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint8_t  uint_fast8_t;
typedef uint32_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;

/* Pointer-sized — use the compiler's own width macros so this header is
 * correct on both RV32 (4-byte) and RV64 (8-byte) pointers. */
typedef __INTPTR_TYPE__  intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

/* Max-width */
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

/* Limits */
#define INT8_MIN    (-128)
#define INT8_MAX    (127)
#define INT16_MIN   (-32768)
#define INT16_MAX   (32767)
#define INT32_MIN   (-2147483648)
#define INT32_MAX   (2147483647)
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   (9223372036854775807LL)

#define UINT8_MAX   (255U)
#define UINT16_MAX  (65535U)
#define UINT32_MAX  (4294967295U)
#define UINT64_MAX  (18446744073709551615ULL)

#define INTPTR_MIN  (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX  __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__

#define SIZE_MAX    __SIZE_MAX__
#define PTRDIFF_MIN (-__PTRDIFF_MAX__ - 1)
#define PTRDIFF_MAX __PTRDIFF_MAX__

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
