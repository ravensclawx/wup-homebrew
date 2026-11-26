/* The flash, as libwup and the menu reach it: through Nintendo's flash driver (its handle; buffers DMA-reachable and
 * uncached: the bounce buffers), or with WUP_KERNEL through the wup-homebrew kernel (include/wupk.h). One set of names
 * for both: FL_READ / FL_WRITE (handle, flash address, buffer, length, &done), FL_ERASE4K (handle, first sector, count). */
#pragma once
#include <stdint.h>
#ifdef WUP_KERNEL
#include "wupk.h"
static inline int FL_READ(uint32_t h, uint32_t a, void *b, uint32_t n, uint32_t *done) { (void)h; *done = n; return WUPK->flash_read(a, b, n); }
static inline int FL_WRITE(uint32_t h, uint32_t a, void *b, uint32_t n, uint32_t *done) { (void)h; *done = n; return WUPK->flash_write(a, b, n); }
static inline int FL_ERASE4K(uint32_t h, uint32_t first, uint32_t count) {
    (void)h; for (uint32_t i = 0; i < count; i++) if (WUPK->flash_erase4k((first + i) << 12)) return -1;
    return 0;
}
#define HANDLE 0
#define FL_HANDLE 0
#else
typedef int (*wup_fl_rw_fn)(uint32_t h, uint32_t addr, void *buf, uint32_t len, uint32_t *done);
typedef int (*wup_fl_er_fn)(uint32_t h, uint32_t first, uint32_t count);
#define FL_READ    ((wup_fl_rw_fn)0x00009141)
#define FL_WRITE   ((wup_fl_rw_fn)0x00008fb3)
#define FL_ERASE4K ((wup_fl_er_fn)0x000092b9)
#define HANDLE     (*(volatile uint32_t *)0x000bb5f4)
#define FL_HANDLE  HANDLE
#endif
