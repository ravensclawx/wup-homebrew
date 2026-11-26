/* The MMU: one flat map of 1 MB sections. RAM 0-4 MB cached (write-back), except 2-4 MB write-through (the
 * framebuffers, ram.h); the peripherals (0xE0000000 SD/Wi-Fi, 0xF0000000 the rest) uncached. Nothing else mapped. */
#include "os.h"

#define SEC(pa, cb) ((pa) | 3u << 10 | 1u << 4 | (cb) | 2u)    /* AP read/write, domain 0, bit 4 (ARM926: set) */
#define WB 0xcu
#define WT 0x8u

void mmu_init(void) {
    u32 *t = (u32 *)_ttb;
    memset(t, 0, 16384);
    for (u32 mb = 0; mb < 4; mb++) t[mb] = SEC(mb << 20, mb >= 2 ? WT : WB);
    t[0xe00] = SEC(0xe0000000u, 0);
    t[0xf00] = SEC(0xf0000000u, 0);
    __asm__ volatile(
        "mcr p15, 0, %0, c7, c10, 4\n"       /* drain the write buffer */
        "mcr p15, 0, %1, c2, c0, 0\n"        /* page table */
        "mcr p15, 0, %2, c3, c0, 0\n"        /* domain 0: manager */
        "mcr p15, 0, %0, c8, c7, 0\n"        /* TLBs */
        "mcr p15, 0, %0, c7, c7, 0\n"        /* caches */
        "mrc p15, 0, r0, c1, c0, 0\n"
        "orr r0, r0, #0x1000\n"              /* I-cache */
        "orr r0, r0, #0x5\n"                 /* MMU, D-cache */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "nop\n nop\n"
        :: "r"(0), "r"((u32)t), "r"(3) : "r0", "memory");
}
void dsb(void) { __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory"); }
void dcache_clean(const void *p, u32 n) {
    for (u32 a = (u32)p & ~31u, e = (u32)p + n; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    dsb();
}
