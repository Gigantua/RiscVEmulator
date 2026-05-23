/* sys/types.h — bare-metal stub. */
#ifndef RVEMU_SYS_TYPES_H
#define RVEMU_SYS_TYPES_H
#include <stdint.h>

typedef int          ssize_t;
typedef int          off_t;
typedef int          pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef unsigned int dev_t;
typedef unsigned int ino_t;
typedef unsigned int nlink_t;
typedef long         time_t;

#endif
