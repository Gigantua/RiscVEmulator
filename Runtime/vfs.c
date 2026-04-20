/* vfs.c — In-memory virtual file system for bare-metal RV32I.
 *
 * Provides file I/O callbacks for PureDoom's doom_set_file_io().
 * Files are pre-registered at known memory addresses by the host
 * (e.g., WAD loaded into RAM by the C# emulator).
 *
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O3 -c
 */

typedef unsigned int u32;

#define VFS_MAX_FILES 8

struct vfs_file {
    const char *name;
    unsigned char *data;
    u32 size;
    u32 pos;
    int in_use;
};

static struct vfs_file vfs_files[VFS_MAX_FILES];

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Find the last component of a path (after last '/' or '\') */
static const char *basename(const char *path)
{
    const char *last = path;
    while (*path) {
        if (*path == '/' || *path == '\\')
            last = path + 1;
        path++;
    }
    return last;
}

void vfs_register(const char *name, void *data, u32 size)
{
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!vfs_files[i].name) {
            vfs_files[i].name   = name;
            vfs_files[i].data   = (unsigned char *)data;
            vfs_files[i].size   = size;
            vfs_files[i].pos    = 0;
            vfs_files[i].in_use = 0;
            return;
        }
    }
}

void *vfs_open(const char *filename, const char *mode)
{
    (void)mode; /* we ignore mode — everything is read-only for now */
    const char *base = basename(filename);

    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (vfs_files[i].name && str_eq(basename(vfs_files[i].name), base)) {
            vfs_files[i].pos    = 0;
            vfs_files[i].in_use = 1;
            return (void *)&vfs_files[i];
        }
    }
    return (void *)0;
}

void vfs_close(void *handle)
{
    if (!handle) return;
    struct vfs_file *f = (struct vfs_file *)handle;
    f->in_use = 0;
    f->pos = 0;
}

int vfs_read(void *handle, void *buf, int count)
{
    if (!handle || count <= 0) return 0;
    struct vfs_file *f = (struct vfs_file *)handle;
    u32 avail = f->size - f->pos;
    u32 n = (u32)count < avail ? (u32)count : avail;

    unsigned char *dst = (unsigned char *)buf;
    unsigned char *src = f->data + f->pos;
    for (u32 i = 0; i < n; i++) dst[i] = src[i];

    f->pos += n;
    return (int)n;
}

int vfs_write(void *handle, const void *buf, int count)
{
    /* Read-only VFS — silently discard writes */
    (void)handle; (void)buf;
    return count;
}

/* origin values: 0=SET, 1=CUR, 2=END (matches doom_seek_t enum) */
int vfs_seek(void *handle, int offset, unsigned int origin)
{
    if (!handle) return -1;
    struct vfs_file *f = (struct vfs_file *)handle;
    int new_pos;

    if (origin == 0)      new_pos = offset;            /* SEEK_SET */
    else if (origin == 1)  new_pos = (int)f->pos + offset;  /* SEEK_CUR */
    else if (origin == 2)  new_pos = (int)f->size + offset;  /* SEEK_END */
    else return -1;

    if (new_pos < 0) new_pos = 0;
    if ((u32)new_pos > f->size) new_pos = (int)f->size;
    f->pos = (u32)new_pos;
    return 0;
}

int vfs_tell(void *handle)
{
    if (!handle) return -1;
    return (int)((struct vfs_file *)handle)->pos;
}

int vfs_eof(void *handle)
{
    if (!handle) return 1;
    struct vfs_file *f = (struct vfs_file *)handle;
    return f->pos >= f->size ? 1 : 0;
}
