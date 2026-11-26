/* Heaps: first-fit free lists, 8-byte aligned blocks with a size word in front; neighbours merge on free.
 * Two: the kernel's (kmalloc, from _heap_start after the stacks to the app region, ram.h) and the running app's
 * (heap_reset over what its image leaves of the app region, app.c). */
#include "os.h"

typedef struct blk { u32 size; struct blk *next; } blk;  /* size includes the header; next only while free */

void heap_reset(heap_t *h, u32 start, u32 end) {
    u32 a = (start + 7) & ~7u;
    blk *b = (blk *)a;
    b->size = (end & ~7u) - a;
    b->next = NULL;
    h->free = b;
}

void *heap_alloc(heap_t *h, u32 n) {
    u32 need = (n + 8 + 7) & ~7u;
    if (need < n) return NULL;
    for (blk **pp = (blk **)&h->free, *b; (b = *pp); pp = &b->next) {
        if (b->size < need) continue;
        if (b->size - need >= 32) {                          /* split: the rest stays free */
            blk *r = (blk *)((u8 *)b + need);
            r->size = b->size - need; r->next = b->next;
            *pp = r; b->size = need;
        } else *pp = b->next;
        return (u8 *)b + 8;
    }
    return NULL;
}

void heap_free(heap_t *h, void *p) {
    if (!p) return;
    blk *b = (blk *)((u8 *)p - 8), **pp = (blk **)&h->free;
    while (*pp && *pp < b) pp = &(*pp)->next;                /* (kept in address order) */
    b->next = *pp; *pp = b;
    if (b->next && (u8 *)b + b->size == (u8 *)b->next) { b->size += b->next->size; b->next = b->next->next; }
    blk *prev = (blk *)h->free;
    if (prev != b) {
        while (prev->next != b) prev = prev->next;
        if ((u8 *)prev + prev->size == (u8 *)b) { prev->size += b->size; prev->next = b->next; }
    }
}

/* free bytes in all, and the largest block */
u32 heap_avail(heap_t *h, u32 *largest) {
    u32 sum = 0, big = 0;
    for (blk *b = (blk *)h->free; b; b = b->next) { sum += b->size - 8; if (b->size - 8 > big) big = b->size - 8; }
    if (largest) *largest = big;
    return sum;
}

static heap_t kheap;
static void kinit(void) { if (!kheap.free) heap_reset(&kheap, (u32)_heap_start, KERNEL_END); }
void *kmalloc(size_t n) { kinit(); return heap_alloc(&kheap, n); }
void kfree(void *p) { heap_free(&kheap, p); }
u32 kheap_free(u32 *largest) { kinit(); return heap_avail(&kheap, largest); }
