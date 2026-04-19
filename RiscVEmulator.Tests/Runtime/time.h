/* time.h — Minimal time support for bare-metal RV32I */
#pragma once

#define CLOCKS_PER_SEC 1000000

typedef long time_t;
typedef long clock_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#ifndef _STRUCT_TIMEVAL_DEFINED
#define _STRUCT_TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

struct tm {
    int tm_sec;    /* seconds [0,60] */
    int tm_min;    /* minutes [0,59] */
    int tm_hour;   /* hours [0,23] */
    int tm_mday;   /* day of month [1,31] */
    int tm_mon;    /* month of year [0,11] */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* day of week [0,6] (Sunday=0) */
    int tm_yday;   /* day of year [0,365] */
    int tm_isdst;  /* daylight savings flag */
};

time_t      time(time_t *t);
clock_t     clock(void);
struct tm  *localtime(const time_t *timep);
struct tm  *gmtime(const time_t *timep);
char       *asctime(const struct tm *tm);
char       *ctime(const time_t *timep);
time_t      mktime(struct tm *tm);
