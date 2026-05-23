/* sys/stat.h — bare-metal stub. */
#ifndef RVEMU_SYS_STAT_H
#define RVEMU_SYS_STAT_H

struct stat {
    unsigned int st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid;
    unsigned int st_rdev, st_size;
    unsigned int st_atime, st_mtime, st_ctime;
};
static inline int stat (const char *p, struct stat *s) { (void)p;(void)s; return -1; }
static inline int lstat(const char *p, struct stat *s) { (void)p;(void)s; return -1; }
static inline int fstat(int fd,    struct stat *s) { (void)fd;(void)s; return -1; }
static inline int mkdir(const char *p, unsigned int mode) { (void)p;(void)mode; return -1; }

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)

#endif
