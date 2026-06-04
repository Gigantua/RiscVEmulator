/* dirent.h — bare-metal stub. Quake only uses opendir/readdir for the
 * file-system scan that lists pak files; we hardcode a single entry. */
#ifndef RVEMU_DIRENT_H
#define RVEMU_DIRENT_H

typedef struct { int fake; } DIR;
struct dirent { unsigned int d_ino; unsigned short d_reclen; unsigned char d_type; char d_name[256]; };

static inline DIR  *opendir (const char *p) { (void)p; return 0; }
static inline struct dirent *readdir (DIR *d) { (void)d; return 0; }
static inline int   closedir(DIR *d) { (void)d; return 0; }

#endif
