#pragma once

#ifndef _STRUCT_TIMEVAL_DEFINED
#define _STRUCT_TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* Stub gettimeofday — runtime provides a real one in syscalls.c */
int gettimeofday(struct timeval *tv, struct timezone *tz);
