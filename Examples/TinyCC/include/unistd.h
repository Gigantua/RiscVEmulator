#pragma once

typedef int ssize_t;

extern char **environ;

/* Stub getcwd: TCC checks for the cwd when searching for headers */
static inline char *getcwd(char *buf, unsigned int size) {
    if (buf && size > 0) { buf[0] = '.'; buf[1] = '\0'; }
    return buf;
}

/* Stub access: TCC calls this to test if a file exists */
static inline int access(const char *path, int mode) {
    (void)path; (void)mode;
    return -1;
}

/* Minimal POSIX defines used by TCC */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
