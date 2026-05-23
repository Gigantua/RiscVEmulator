/* Minimal cutils.h replacement for the bare-metal rvemu build.
 *
 * Bellard's softfp_template.h uses these macros from upstream
 * cutils.h. We define just what's needed (the upstream cutils is
 * Linux-host-targeted and pulls in a lot of unrelated headers we
 * don't have in the Runtime/ bare-metal world). */
#ifndef CUTILS_H
#define CUTILS_H

#include <stdint.h>

#define glue3(a, b)   a##b
#define glue(a, b)    glue3(a, b)
#define unlikely(x)   __builtin_expect((x), 0)
#define likely(x)     __builtin_expect((x), 1)

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#endif
