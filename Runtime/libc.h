/* libc.h — Convenience header: includes the entire minimal C standard library.
 *
 * All output goes through _write(1, ...) which is implemented in syscalls.c.
 *
 * Usage: #include "libc.h"
 * Link:  libc.o (plus runtime.o, syscalls.o, malloc.o)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <time.h>
