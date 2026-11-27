/* malloc for large apps (app.cfg: large = yes): TLSF (third_party/tlsf, BSD) over every piece of RAM such an app may
 * have, as one heap: its own (from its .bss to WUP_APP_END), and screen 0 (820 KB), which a large app does not need:
 * it shows screen 1 alone (gfx.c: no double buffering). The sound and microphone rings between them stay where the
 * kernel keeps them. TLSF takes several pools and keeps its time constant and its fragmentation low.
 *   These replace the C library's malloc family (picolibc's are not linked then). Threads: one lock, a kernel
 *   semaphore. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "tlsf/tlsf.h"
#include "wup_ram.h"
#include "wupk.h"

extern char __bss_end[];
static tlsf_t heap;
static int lock_sem;
static size_t total;

static void init(void) {
    uintptr_t lo = ((uintptr_t)__bss_end + 63) & ~(uintptr_t)63;
    heap = tlsf_create_with_pool((void *)lo, WUP_APP_END - lo);
    tlsf_add_pool(heap, (void *)WUP_FB0, WUP_FB_SIZE);
    total = (WUP_APP_END - lo) + WUP_FB_SIZE;
    lock_sem = WUPK->sem_new(1, 1);
}
static void lock(void) { if (!heap) init(); WUPK->sem_wait(lock_sem, -1); }
static void unlock(void) { WUPK->sem_signal(lock_sem); }

void *malloc(size_t n) {
    lock(); void *p = tlsf_malloc(heap, n ? n : 1); unlock();
    if (!p) errno = ENOMEM;
    return p;
}
void free(void *p) { if (!p) return; lock(); tlsf_free(heap, p); unlock(); }
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    lock(); void *q = tlsf_realloc(heap, p, n); unlock();
    if (!q) errno = ENOMEM;
    return q;
}
void *calloc(size_t a, size_t b) {
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) { errno = ENOMEM; return NULL; }
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *memalign(size_t align, size_t n) {
    lock(); void *p = tlsf_memalign(heap, align, n ? n : 1); unlock();
    if (!p) errno = ENOMEM;
    return p;
}
void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }
int posix_memalign(void **out, size_t align, size_t n) {
    void *p = memalign(align, n);
    if (!p) return ENOMEM;
    *out = p; return 0;
}
size_t malloc_usable_size(void *p) { return p ? tlsf_block_size(p) : 0; }

/* what is free (libwup's wup_mem_free, for large apps): every free block of both pools */
static void add_free(void *ptr, size_t size, int used, void *user) { (void)ptr; if (!used) *(size_t *)user += size; }
size_t wup__large_free(void) {
    size_t n = 0;
    lock();
    tlsf_walk_pool(tlsf_get_pool(heap), add_free, &n);
    /* (the second pool: TLSF keeps no list of pools; it starts at screen 0, after the pool header) */
    tlsf_walk_pool((pool_t)WUP_FB0, add_free, &n);
    unlock();
    return n;
}
size_t wup__large_total(void) { if (!heap) { lock(); unlock(); } return total; }
