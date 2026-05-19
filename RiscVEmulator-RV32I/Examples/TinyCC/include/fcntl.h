#pragma once

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* Stub open: bare-metal, no filesystem */
static inline int open(const char *path, int flags, ...) {
    (void)path; (void)flags;
    return -1;
}
