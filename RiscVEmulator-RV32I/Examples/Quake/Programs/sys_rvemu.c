/* sys_rvemu.c — System glue for bare-metal RV32I Quake.
 *
 *   - Sys_FloatTime: RTC microseconds.
 *   - Sys_File*: DiskDevice-backed virtual file (pak0.pak on host).
 *   - Sys_Error / Sys_Quit: print + exit via HostExitDevice.
 *   - Sys_Printf: UART.
 *   - Sys_Init / Sys_Sleep: stubs.
 */

#include <stdarg.h>
#include "quakedef.h"
#include "common.h"
#include "libc.h"

#define UART_THR     (*(volatile unsigned char *)0x10000000)
#define RTC_US_LO    (*(volatile unsigned int  *)0x10003000)
#define RTC_US_HI    (*(volatile unsigned int  *)0x10003004)
#define HOST_EXIT    (*(volatile unsigned int  *)0x40000000)

#define DISK_OFFSET_LO (*(volatile unsigned int *)0x10004000)
#define DISK_OFFSET_HI (*(volatile unsigned int *)0x10004004)
#define DISK_LENGTH    (*(volatile unsigned int *)0x10004008)
#define DISK_DEST_ADDR (*(volatile unsigned int *)0x1000400C)
#define DISK_CMD       (*(volatile unsigned int *)0x10004010)
#define DISK_STATUS    (*(volatile unsigned int *)0x10004014)
#define DISK_FSIZE_LO  (*(volatile unsigned int *)0x10004018)

/* ── Virtual file table ──────────────────────────────────────────
 * We expose a single backing file — the pak0 image the host put on
 * disk. Quake calls Sys_FileOpenRead("id1/pak0.pak", &h) and we hand
 * out handle 0 with the host disk size.
 */
typedef struct {
    int  open;
    unsigned int pos;
    unsigned int size;
} vfile_t;
static vfile_t vfiles[8];

void
Sys_Printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (const char *p = buf; *p; p++) UART_THR = (unsigned char)*p;
}

void
Sys_Error(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Sys_Printf("FATAL: %s\n", buf);
    HOST_EXIT = 1;
    for (;;) { }
}

void
Sys_Quit(void)
{
    Sys_Printf("\nGoodbye.\n");
    HOST_EXIT = 0;
    for (;;) { }
}

double
Sys_DoubleTime(void)
{
    /* Microseconds since boot. Quake wants double seconds. */
    unsigned int lo = RTC_US_LO;
    unsigned int hi = RTC_US_HI;
    unsigned long long us = ((unsigned long long)hi << 32) | lo;
    return (double)us * (1.0 / 1000000.0);
}
float Sys_FloatTime(void) { return (float)Sys_DoubleTime(); }

void Sys_Init(void)         { for (int i = 0; i < 8; i++) vfiles[i].open = 0; }
void Sys_Sleep(void)        { }
void Sys_LowFPPrecision(void)  { }
void Sys_HighFPPrecision(void) { }
void Sys_SetFPCW(void)         { }

/* ── File system: only pak0 is backed; everything else is "not found". */
static int
fname_is_pak0(const char *path)
{
    /* Accept "id1/pak0.pak", "./id1/pak0.pak", etc. */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    return strcmp(base, "pak0.pak") == 0;
}

int
Sys_FileOpenRead(const char *path, int *handle)
{
    if (!fname_is_pak0(path)) { *handle = -1; return -1; }
    for (int i = 0; i < 8; i++) {
        if (!vfiles[i].open) {
            vfiles[i].open = 1;
            vfiles[i].pos  = 0;
            vfiles[i].size = DISK_FSIZE_LO;
            *handle = i;
            return (int)vfiles[i].size;
        }
    }
    *handle = -1;
    return -1;
}

int  Sys_FileOpenWrite(const char *path) { (void)path; return -1; }
void Sys_FileClose    (int handle) { if (handle >= 0 && handle < 8) vfiles[handle].open = 0; }
void Sys_FileSeek     (int handle, int position)
{
    if (handle >= 0 && handle < 8 && vfiles[handle].open)
        vfiles[handle].pos = (unsigned int)position;
}

int
Sys_FileRead(int handle, void *dst, int count)
{
    if (handle < 0 || handle >= 8 || !vfiles[handle].open) return 0;
    vfile_t *f = &vfiles[handle];
    if (f->pos >= f->size) return 0;
    unsigned int rem = f->size - f->pos;
    if ((unsigned int)count > rem) count = (int)rem;

    DISK_OFFSET_LO = f->pos;
    DISK_OFFSET_HI = 0;
    DISK_LENGTH    = (unsigned int)count;
    DISK_DEST_ADDR = (unsigned int)(unsigned long)dst;
    DISK_CMD       = 1;
    while (DISK_STATUS != 0) { /* poll */ }
    f->pos += (unsigned int)count;
    return count;
}

int  Sys_FileWrite(int handle, const void *src, int count) { (void)handle;(void)src;(void)count; return 0; }
int  Sys_FileTime (const char *path) { (void)path; return -1; }
void Sys_mkdir    (const char *path) { (void)path; }
void Sys_DebugLog (const char *file, const char *fmt, ...) { (void)file;(void)fmt; }

/* Quake on UNIX calls Sys_ConsoleInput once per frame for stdin tty input;
 * bare-metal has no console, so we return nothing. */
char *Sys_ConsoleInput(void) { return 0; }

/* Optional clipboard hooks — stubs. */
char *Sys_GetClipboardData(void) { return 0; }
void  Sys_CopyToClipboard(const char *text) { (void)text; }
