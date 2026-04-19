/* errno.h — Error codes for bare-metal RV32I */
#pragma once

extern int errno;

#define EIO    5
#define EBADF  9
#define ENOMEM 12
#define EINVAL 22
#define EDOM   33
#define ERANGE 34
#define ENOSYS 38
