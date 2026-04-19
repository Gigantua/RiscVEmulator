/* malloc.c — Minimal heap allocator for bare-metal RV32I.
 *
 * Uses a first-fit freelist with 8-byte block headers.
 * Heap grows upward from _heap_start (provided by linker.ld).
 * No coalescing on free (keeps it simple; Doom's zone allocator
 * manages its own memory on top of this anyway).
 *
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O3 -c
 */

#include <stdlib.h>

typedef unsigned int u32;

/* Linker-provided symbols */
extern char _heap_start;

/* Block header: 8 bytes (size + next-free pointer) */
struct block_hdr {
    u32 size;           /* payload size (not counting header) */
    struct block_hdr *next; /* next free block, or 0 if allocated */
};

#define HDR_SIZE   ((u32)sizeof(struct block_hdr))
#define ALIGN(x)   (((x) + 7u) & ~7u)

/* Globals: freelist head and bump pointer */
static struct block_hdr *free_list = 0;
static char *heap_brk = 0;
static char *heap_base = 0;

/* Heap ceiling — 8 MB above _heap_start (WAD lives at 10 MB, stack at top of RAM) */
#define HEAP_LIMIT  (8u * 1024u * 1024u)

static void init_heap(void)
{
    if (!heap_base) {
        heap_base = &_heap_start;
        heap_brk  = heap_base;
    }
}

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    init_heap();

    u32 need = ALIGN((u32)size);

    /* Search freelist (first-fit) */
    struct block_hdr **pp = &free_list;
    while (*pp) {
        struct block_hdr *blk = *pp;
        if (blk->size >= need) {
            /* Unlink from freelist */
            *pp = blk->next;
            blk->next = 0; /* mark as allocated */
            return (void *)((char *)blk + HDR_SIZE);
        }
        pp = &(blk->next);
    }

    /* Bump allocate */
    u32 total = HDR_SIZE + need;
    if ((u32)(heap_brk - heap_base) + total > HEAP_LIMIT)
        return (void *)0; /* out of memory */

    struct block_hdr *blk = (struct block_hdr *)heap_brk;
    blk->size = need;
    blk->next = 0; /* allocated */
    heap_brk += total;

    return (void *)((char *)blk + HDR_SIZE);
}

void free(void *ptr)
{
    if (!ptr) return;
    struct block_hdr *blk = (struct block_hdr *)((char *)ptr - HDR_SIZE);
    /* Prepend to freelist */
    blk->next = free_list;
    free_list = blk;
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = malloc(total);
    if (p) {
        unsigned char *d = (unsigned char *)p;
        for (size_t i = 0; i < total; i++) d[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t new_size)
{
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }

    struct block_hdr *blk = (struct block_hdr *)((char *)ptr - HDR_SIZE);
    u32 old_size = blk->size;
    if (new_size <= old_size) return ptr; /* fits already */

    void *newp = malloc(new_size);
    if (newp) {
        unsigned char *d = (unsigned char *)newp;
        unsigned char *s = (unsigned char *)ptr;
        u32 copy = old_size < (u32)new_size ? old_size : (u32)new_size;
        for (u32 i = 0; i < copy; i++) d[i] = s[i];
        free(ptr);
    }
    return newp;
}
