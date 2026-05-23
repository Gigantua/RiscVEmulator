/* rvemu_config.h — Compile-time configuration for the bare-metal RV32I
 * Quake port. Included via clang -include before every TyrQuake TU.
 *
 * Goals:
 *   - Pretend to be a generic "embedded UNIX" so TyrQuake's #ifdefs pick
 *     the portable code paths (no Win32, no X11, no SDL, no OpenGL).
 *   - Silence/shim a handful of libc declarations TyrQuake expects but
 *     our Runtime/libc.c doesn't expose verbatim.
 */
#ifndef RVEMU_CONFIG_H
#define RVEMU_CONFIG_H

/* Pre-define guards so clang's resource-dir wrappers (which #include_next
 * to a sysroot we don't have) skip themselves. We then ship the types we
 * actually need from these headers via Runtime/libc.h or via stdint.h. */
#define __CLANG_INTTYPES_H
#define __need_int_types
#include <stdint.h>             /* clang builtin, self-contained */

typedef int8_t   int_least8_t;
typedef uint8_t  uint_least8_t;
typedef int16_t  int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t  int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t  int_least64_t;
typedef uint64_t uint_least64_t;
typedef long long   intmax_t;
typedef unsigned long long uintmax_t;

/* Tell TyrQuake we have no GL, no OS-specific drivers. Note: TyrQuake
 * uses `#ifdef` (presence), so we must NOT define SERVERONLY or QW_HACK. */
#define NQ_HACK         1
#define _BSD            0
#undef  _WIN32
#undef  _WIN64
#undef  WIN32
#undef  __linux__

/* Word size. */
#define id386           0       /* no x86 inline asm */

/* Suppress TyrQuake's "host OS" auto-detection by force-defining the
 * names it switches on. */
#define PLATFORM_RVEMU  1

/* tyrquake calls strcasecmp / strncasecmp; we provide both names as
 * real functions now (string.h declares them; stubs_rvemu.c implements
 * them as forwarders to stricmp/strnicmp). No macro needed. */

/* Standard stdio constants Runtime/stdio.h doesn't define. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Quake's stat/file-time call sites pass errno; bare-metal Runtime/errno.h
 * has errno via a global int — set EBUSY etc. minimally. */
#ifndef EBUSY
#define EBUSY  16
#endif
#ifndef ENOENT
#define ENOENT 2
#endif

/* qsort / atoi / atof live in our libc / stdlib headers — but tyrquake's
 * common.h does NOT pull in <stdlib.h>, so without an explicit prototype
 * `atof` gets the implicit-int declaration. That makes clang emit
 * `__floatsisf(a0)` (int→float) after `jal atof`, mangling every
 * `((float *)d)[i] = atof(token);` in ED_ParseEpair into zero — every
 * entity origin parses as (0,0,0), spawn lands outside the map. */
double atof(const char *s);
int    atoi(const char *s);
long   atol(const char *s);

/* Network byte order helpers — tyrquake's NQ/net_main.c calls htons/ntohs
 * but `#include <arpa/inet.h>` doesn't exist on bare metal. Implementations
 * are identity functions in stubs_rvemu.c (we don't do real TCP/IP). */
unsigned short htons(unsigned short x);
unsigned short ntohs(unsigned short x);
unsigned int   htonl(unsigned int   x);
unsigned int   ntohl(unsigned int   x);

/* Mark functions our libc doesn't actually have so weak external linkage
 * doesn't drag them in from elsewhere. */

/* alloca MUST be a macro, not a wrapper function. `__builtin_alloca`
 * allocates on the CURRENT function's stack — wrapping it inside a
 * function returns a pointer to memory that's freed when the wrapper
 * returns. That dangling pointer broke TyrQuake's `R_RenderView_`
 * (which uses alloca for the edge/surface arrays). Force-define here
 * so every TyrQuake TU that uses `alloca(N)` expands to a compiler-
 * builtin in its own frame. */
#define alloca(sz) __builtin_alloca(sz)

#endif /* RVEMU_CONFIG_H */
