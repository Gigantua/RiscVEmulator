#pragma once

/* PROT_* may already be defined in config.h */
#ifndef PROT_READ
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#endif

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANON    0x20

#define MAP_FAILED  ((void *)-1)

/* mmap stub: bare-metal has no virtual memory subsystem */
static inline void *mmap(void *addr, unsigned int length, int prot, int flags, int fd, long offset)
{
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    return MAP_FAILED;
}

static inline int munmap(void *addr, unsigned int length)
{
    (void)addr; (void)length;
    return -1;
}
