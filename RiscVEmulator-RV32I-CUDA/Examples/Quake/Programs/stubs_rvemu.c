/* stubs_rvemu.c — fill-in symbols Quake's link references but we don't
 * actually need on bare-metal (network protocol, music decoders, exotic
 * sound functions). Most are no-ops that just satisfy the linker.
 */

#include "quakedef.h"
#include "client.h"
#include "host.h"
#include "net.h"

/* Network: with net_main.c + net_loop.c + net_none.c in the build, all
 * NET_* and Loop_* are provided. No stubs needed here. */

/* Sound API now provided by snd_dma.c. */

/* ── Music (CD/MP3/OGG): nothing. */
void CDAudio_Play  (byte track, qboolean looping)                { (void)track;(void)looping; }
void CDAudio_Stop  (void)                                        { }
void CDAudio_Pause (void)                                        { }
void CDAudio_Resume(void)                                        { }
int  CDAudio_Init  (void)                                        { return -1; }
void CDAudio_Shutdown(void)                                      { }
void CDAudio_Update(void)                                        { }

/* __floatdidf / __floatundidf now come from Runtime/softfloat.c
 * (Bellard softfp via cvt_i64_sf64 / cvt_u64_sf64). The old
 * hand-rolled hi*2^32 + lo shims lost precision for large ints. */

/* ── math: modff. */
float modff(float x, float *iptr)
{
    int i = (int)x;
    *iptr = (float)i;
    return x - (float)i;
}

/* ── time stubs. Quake uses gmtime_r/strftime for the QuickSave
 * filename; we just write a generic placeholder. */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
            tm_wday, tm_yday, tm_isdst; };
struct tm *gmtime_r(const long *t, struct tm *r) { (void)t; *r = (struct tm){0}; return r; }
unsigned int strftime(char *s, unsigned int max, const char *fmt, const struct tm *tm)
{
    (void)fmt;(void)tm;
    const char *p = "rv32";
    unsigned int i = 0;
    while (i + 1 < max && p[i]) { s[i] = p[i]; i++; }
    if (max) s[i] = 0;
    return i;
}

/* ── fprintf to stderr: route to UART. We ignore the FILE* parameter. */
#include <stdarg.h>
#include <stdio.h>
extern void Sys_Printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Sys_Printf("%s", buf);
    return n;
}

void Sys_SendKeyEvents(void) { /* polled from IN_ProcessKeyboard in quake_main */ }

/* alloca MUST NOT be wrapped in a function — `__builtin_alloca` allocates
 * on the CURRENT function's stack, so wrapping it returns a pointer to
 * memory freed when the wrapper returns. The pointer is then dangling
 * and any subsequent stack write clobbers what the caller stored there.
 *
 * For TyrQuake this manifested as `R_RenderView_` calling `alloca` for
 * the edge/surface arrays, getting a dangling pointer, and the very
 * first Sys_Printf trace overwriting `surfaces[1]` (the sentinel) with
 * stack noise — turning R_LeadingEdge's sort walk into an infinite
 * cycle that the runaway guards just papered over.
 *
 * Real fix: alloca has to expand inline in the caller. Defined as a
 * macro in rvemu_config.h so every TU that #includes that header sees
 * the macro form before the `<stdlib.h>` prototype-style declaration. */

/* High-level fopen/fread family. Quake uses these to access pak0.pak via
 * COM_FOpenFile; we route them to DiskDevice for pak0.pak and return
 * "not found" for everything else (no writable storage on bare-metal). */
#define DISK_OFFSET_LO (*(volatile unsigned int *)0x10004000)
#define DISK_OFFSET_HI (*(volatile unsigned int *)0x10004004)
#define DISK_LENGTH    (*(volatile unsigned int *)0x10004008)
#define DISK_DEST_ADDR (*(volatile unsigned int *)0x1000400C)
#define DISK_CMD       (*(volatile unsigned int *)0x10004010)
#define DISK_STATUS    (*(volatile unsigned int *)0x10004014)
#define DISK_FSIZE_LO_ (*(volatile unsigned int *)0x10004018)

/* RVFile.kind: 0=pak0 (DiskDevice MMIO), 1=virtual (vdata pointer). */
typedef struct {
    int open;
    int kind;
    unsigned int pos;
    unsigned int size;
    const char *vdata;
} RVFile;
static RVFile rvfiles[8];

/* Virtual autoexec.cfg — Quake's Cmd_Exec_f calls fopen+fread on this
 * during Host_Init right after running quake.rc. We don't have a real
 * filesystem on bare metal, so we serve a compiled-in string that sets
 * up modern-FPS bindings + sensible cvars. tyrquake parses it with the
 * same lexer that handles quake.rc — `bind` / cvar names / `\n`. */
static const char autoexec_cfg[] =
    "bind w +forward\n"
    "bind s +back\n"
    "bind a +moveleft\n"
    "bind d +moveright\n"
    "bind SPACE +jump\n"
    "bind CTRL +attack\n"
    "bind SHIFT +speed\n"
    "bind MOUSE1 +attack\n"
    "bind MOUSE2 +forward\n"
    "bind ` toggleconsole\n"
    "bind ~ toggleconsole\n"
    /* Don't bind 'y' — the quit-menu reads K_y directly from M_Quit_Key,
     * and a binding would steal the keypress before the menu sees it. */
    "lookspring 0\n"
    "lookstrafe 0\n"
    "m_freelook 1\n"
    "m_pitch 0.022\n"
    "m_yaw 0.022\n"
    "sensitivity 3\n"
;

static const char *basename_of(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

/* True if `base` matches `name` up to the first '"' or end-of-string
 * (Quake sometimes appends trailing junk during path concat). */
static int base_matches(const char *base, const char *name)
{
    int i;
    for (i = 0; name[i]; i++)
        if (base[i] != name[i]) return 0;
    return base[i] == 0 || base[i] == '"';
}

FILE *fopen(const char *path, const char *mode)
{
    (void)mode;
    const char *base = basename_of(path);

    /* Virtual config files. We don't ship config.cfg because the engine
     * writes its current state into it on shutdown — but autoexec.cfg
     * is read-only and a perfect fit for our compiled-in bindings. */
    const char *vdata = 0;
    unsigned int vsize = 0;
    if (base_matches(base, "autoexec.cfg")) {
        vdata = autoexec_cfg;
        vsize = (unsigned int)(sizeof(autoexec_cfg) - 1);
        Sys_Printf("[rvemu] virtual fopen autoexec.cfg path=%s size=%u\n",
                   path, vsize);
    }

    /* Real on-disk file: only pak0.pak is supported. */
    int is_pak0 = base_matches(base, "pak0.pak");

    if (!vdata && !is_pak0) return 0;

    for (int i = 0; i < 8; i++) {
        if (!rvfiles[i].open) {
            rvfiles[i].open  = 1;
            rvfiles[i].pos   = 0;
            if (vdata) {
                rvfiles[i].kind  = 1;
                rvfiles[i].vdata = vdata;
                rvfiles[i].size  = vsize;
            } else {
                rvfiles[i].kind  = 0;
                rvfiles[i].vdata = 0;
                rvfiles[i].size  = DISK_FSIZE_LO_;
            }
            return (FILE *)(unsigned long)(i + 1);
        }
    }
    return 0;
}
int fclose(FILE *f)
{
    int h = (int)(unsigned long)f - 1;
    if (h >= 0 && h < 8) rvfiles[h].open = 0;
    return 0;
}
size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
    int h = (int)(unsigned long)f - 1;
    if (h < 0 || h >= 8 || !rvfiles[h].open) return 0;
    RVFile *fp = &rvfiles[h];
    unsigned int total = sz * n;
    if (fp->pos >= fp->size) return 0;
    unsigned int rem = fp->size - fp->pos;
    if (total > rem) total = rem;

    if (fp->kind == 1) {
        /* Virtual content: simple memcpy from the in-image string. */
        const unsigned char *src = (const unsigned char *)(fp->vdata + fp->pos);
        unsigned char *dst = (unsigned char *)p;
        for (unsigned int i = 0; i < total; i++) dst[i] = src[i];
        fp->pos += total;
        return sz ? total / sz : 0;
    }

    /* pak0.pak via DiskDevice MMIO DMA. */
    DISK_OFFSET_LO = fp->pos;
    DISK_OFFSET_HI = 0;
    DISK_LENGTH    = total;
    DISK_DEST_ADDR = (unsigned int)(unsigned long)p;
    DISK_CMD       = 1;
    while (DISK_STATUS != 0) { /* spin */ }
    /* DiskDevice has asynchronously written `total` bytes into `*p`, but
     * the compiler can't see that side-effect — empty asm-clobber forces
     * a reload from any memory used after this point. */
    __asm__ __volatile__("" ::: "memory");
    fp->pos += total;
    return sz ? total / sz : 0;
}
int fseek(FILE *f, long off, int whence)
{
    int h = (int)(unsigned long)f - 1;
    if (h < 0 || h >= 8 || !rvfiles[h].open) return -1;
    RVFile *fp = &rvfiles[h];
    unsigned int newpos;
    switch (whence) {
        case 0: newpos = (unsigned int)off; break;            /* SEEK_SET */
        case 1: newpos = fp->pos + (unsigned int)off; break;  /* SEEK_CUR */
        case 2: newpos = fp->size + (unsigned int)off; break; /* SEEK_END */
        default: return -1;
    }
    fp->pos = newpos;
    return 0;
}
long ftell(FILE *f)
{
    int h = (int)(unsigned long)f - 1;
    if (h < 0 || h >= 8 || !rvfiles[h].open) return -1;
    return (long)rvfiles[h].pos;
}
size_t fwrite(const void *p, size_t s, size_t n, FILE *f)
    { (void)p;(void)s;(void)n;(void)f; return 0; }
int   fflush(FILE *f) { (void)f; return 0; }
int   fscanf(FILE *f, const char *fmt, ...) { (void)f;(void)fmt; return 0; }
int   feof(FILE *f)
{
    int h = (int)(unsigned long)f - 1;
    if (h < 0 || h >= 8 || !rvfiles[h].open) return 1;
    return rvfiles[h].pos >= rvfiles[h].size;
}
int   ferror(FILE *f) { (void)f; return 0; }
int   fgetc(FILE *f)
{
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return -1;
    return (int)c;
}
int   getc(FILE *f) { return fgetc(f); }

/* Bare-metal has no writable user dir; return read-only path "/". */
const char *Sys_UserDataDirectory(void) { return "/"; }

/* Network byte order — we don't actually use the loopback driver for
 * real socket traffic, so identity functions suffice. */
unsigned short ntohs(unsigned short x) { return x; }
unsigned int   ntohl(unsigned int   x) { return x; }
unsigned short htons(unsigned short x) { return x; }
unsigned int   htonl(unsigned int   x) { return x; }

/* Sound API now provided by snd_dma.c (in the link). */
qboolean isDedicated = false;
/* S_Update/S_AddCommands/etc. come from snd_dma.c which is now linked. */
void IN_AddCommands(void)        { }
void IN_RegisterVariables(void)  { }
void CDAudio_AddCommands(void)   { }
void CDAudio_RegisterVariables(void) { }
void Sys_RegisterVariables(void) { }
/* getc defined above with proper FILE* signature. */

/* cvars referenced by snd_dma.c / vid_mode.c but normally registered by
 * other null/platform drivers. */
cvar_t bgmvolume      = { "bgmvolume",      "1", true };
cvar_t _windowed_mouse= { "_windowed_mouse","1", true };
void BGM_ClearBuffers(void)                { }
void BGM_Stop(void)                        { }
void S_Music_AddCommands(void)             { }
void S_Music_RegisterVariables(void)       { }
void S_Music_Init(void)                    { }
void BGM_PlayCDTrack(byte t, qboolean l)   { (void)t;(void)l; }
void S_PaintMusic(portable_samplepair_t *buf, int n) { (void)buf;(void)n; }
struct client_s;
void NET_Ban_f(struct client_s *client)    { (void)client; }

/* strtod / atof — strtod is in Runtime/libc.c. Use the double-precision
 * path: Quake parses BSP entity strings like "-512.125 256 88.5" via
 * atof on each token, and the spawn point lands on the wrong side of
 * a clip plane if fractional precision is lost. */
extern double strtod(const char *s, char **endp);
double atof(const char *s) { return strtod(s, 0); }

/* long-double (TF, 128-bit quad) softfloat helpers.
 *
 * tyrquake/common/model.c:CalcSurfaceExtents casts vertex coords to
 * `(long double)` to gain x87-style extended precision during dot
 * products. Our toolchain has no hardware quad-float, so clang lowers
 * every long-double op to compiler-rt's __addtf3/__multf3/etc.
 *
 * The RV32 ilp32 psABI passes/returns __float128 BY REFERENCE: arg slots
 * a0/a1/a2 hold pointers to 16-byte buffers, NOT the values themselves.
 * The previous stubs in this file declared `tf_t = double` and took
 * args by value — which silently read the result pointer as the float
 * operand and returned garbage in the wrong registers. Symptom on
 * Quake: random `surf->extents[]` → D_SCAlloc 0-size fatal, broken
 * collision planes (player falls through floor / slides sideways).
 *
 * Since the surrounding code immediately truncates the long-double
 * result back to float, full 113-bit quad precision is overkill.
 * We use a SMUGGLED-DOUBLE format internal to these stubs: the
 * 16-byte buffer holds an IEEE binary64 in bytes [0..7] and zero
 * padding in bytes [8..15]. As long as no outside code ever inspects
 * the buffer (Quake never does — it's purely transient), this format
 * is self-consistent and respects the by-reference ABI exactly. */
typedef struct { double val; unsigned long long pad; } tf_buf;

void __extendsftf2(tf_buf *out, float x)               { out->val = (double)x; out->pad = 0; }
void __extenddftf2(tf_buf *out, double x)              { out->val = x;         out->pad = 0; }
float  __trunctfsf2(const tf_buf *a)                   { return (float)a->val; }
double __trunctfdf2(const tf_buf *a)                   { return a->val; }
void __addtf3(tf_buf *o, const tf_buf *a, const tf_buf *b) { o->val = a->val + b->val; o->pad = 0; }
void __subtf3(tf_buf *o, const tf_buf *a, const tf_buf *b) { o->val = a->val - b->val; o->pad = 0; }
void __multf3(tf_buf *o, const tf_buf *a, const tf_buf *b) { o->val = a->val * b->val; o->pad = 0; }
void __divtf3(tf_buf *o, const tf_buf *a, const tf_buf *b) { o->val = a->val / b->val; o->pad = 0; }
void BGM_PlayCDtrack(unsigned int track, int looping)
    { (void)track;(void)looping; }
void BGM_Pause(void)        { }
void BGM_Resume(void)       { }
void BGM_Update(void)       { }
void BGM_Shutdown(void)     { }
int  BGM_Init(void)         { return 0; }

/* ── libc bits Runtime/libc.c doesn't expose ────────────────────── */
int stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strnicmp(const char *a, const char *b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        int ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}
/* POSIX aliases — same impl. Used by both tyrquake (via strcasecmp on
 * Linux include paths) and any non-Windows port. */
int strcasecmp(const char *a, const char *b)             { return stricmp(a, b); }
int strncasecmp(const char *a, const char *b, unsigned int n) { return strnicmp(a, b, n); }

