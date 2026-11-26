/* mmu: make RAM we own cacheable while an app runs, and put the firmware's mapping back afterwards.
 * The stock firmware (rev 7677) maps RAM (TTB 0x3f8000): 0-1 MB section cached; 1-2 MB coarse table
 * at 0x3fc000 (0x100000-0x157fff cached, 0x158000-0x1fffff uncached: its DMA area); 2-3 MB section
 * uncached+unbuffered; 3-4 MB coarse table at 0x3fc400 (uncached). Our code there ran with every
 * instruction fetched from RAM (the I-cache only caches C=1 pages): ~23 MB/s copies vs 44 cached.
 * Only 64 KB "large page" ranges inside coarse tables are changed (16 identical entries each). */
#include "mmu.h"
#ifdef WUP_KERNEL
/* The wup-homebrew kernel maps RAM itself (the first 2 MB cached, the rest write-through: what DMA reads always holds
   the CPU's writes): nothing to change; "uncache" only writes the D-cache back */
__attribute__((target("arm"), noinline)) static void dclean_all(void) {
    __asm__ volatile("1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n\tbne 1b\n\tmcr p15, 0, %0, c7, c10, 4\n\tmcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory", "cc");
}
int mmu_cache(unsigned lo, unsigned hi) { (void)lo; (void)hi; return 0; }
void mmu_uncache(void) { dclean_all(); }
void mmu_uncache_last(void) { dclean_all(); }
int mmu_cache_section(unsigned mb_addr, unsigned hi) { (void)mb_addr; (void)hi; return 0; }
void mmu_uncache_all(void) { dclean_all(); }
#else

typedef unsigned u32;
__attribute__((target("arm"), noinline)) static u32 ttb(void) { u32 v; __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v)); return v & 0xffffc000u; }
__attribute__((target("arm"), noinline)) static void tlb_inval(void) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n\tmcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory");
}
__attribute__((target("arm"), noinline)) static void dclean_inval(u32 a, u32 e) {
    for (a &= ~31u; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n\tmcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");   /* drain, I-cache inval */
}
static volatile u32 *entry(u32 addr) {
    u32 l1 = *(volatile u32 *)(ttb() + (addr >> 20) * 4);
    if ((l1 & 3) != 1) return 0;                                   /* only coarse tables */
    return (volatile u32 *)((l1 & 0xfffffc00u) + ((addr >> 12) & 0xff) * 4);
}
#define MAXR 4
static struct { u32 lo, n, e[16]; } rng[MAXR]; static int nr;
int mmu_cache(u32 lo, u32 hi) {                                    /* 64 KB aligned, up to 1 MB */
    if (nr == MAXR || hi - lo > 0x100000) return -2;
    for (u32 a = lo; a < hi; a += 0x1000) {
        volatile u32 *e = entry(a);
        if (!e || (*e & 3) != 1) return -1;                        /* large pages only */
    }
    rng[nr].lo = lo; rng[nr].n = 0;
    for (u32 a = lo; a < hi; a += 0x10000) rng[nr].e[rng[nr].n++] = *entry(a);
    nr++;
    for (u32 a = lo; a < hi; a += 0x1000) *entry(a) |= 0xc;        /* C=1 B=1: write-back */
    tlb_inval();
    return 0;
}
/* restore only the most recently cached range (not the one holding the current stack) */
void mmu_uncache_last(void) {
    if (!nr) return;
    nr--;
    u32 lo = rng[nr].lo, hi = lo + rng[nr].n * 0x10000;
    dclean_inval(lo, hi);
    for (u32 a = lo; a < hi; a += 0x1000) *entry(a) = rng[nr].e[(a - lo) >> 16];
    tlb_inval();
    dclean_inval(lo, hi);
}
void mmu_uncache(void) {
    while (nr) {
        nr--;
        u32 lo = rng[nr].lo, hi = lo + rng[nr].n * 0x10000;
        dclean_inval(lo, hi);                                      /* nothing may stay only in the cache */
        for (u32 a = lo; a < hi; a += 0x1000) *entry(a) = rng[nr].e[(a - lo) >> 16];
        tlb_inval();
        dclean_inval(lo, hi);
    }
}

/* Part of a 1 MB section (first-level "section" descriptor): 0x200000-0x2fffff, which the firmware maps uncached
   as one entry. Only [mb_addr, hi) becomes cacheable: the section is replaced by a second-level table of 4 KB pages
   that keep its permissions, with C and B set below hi. The rest stays as it was: the firmware's Wi-Fi driver has
   its working memory at the top (0x2FC000..., read and written by the radio's DMA; cached, its commands reached
   the radio stale and the pad stopped with error 165-3423). Restored by mmu_uncache_all. */
static u32 sec_saved[2], sec_mb[2]; static int nsec;
static u32 l2[2][256] __attribute__((aligned(1024)));
int mmu_cache_section(u32 mb_addr, u32 hi) {
    volatile u32 *l1 = (volatile u32 *)(ttb() + (mb_addr >> 20) * 4);
    u32 sec = *l1, mb = mb_addr & 0xfff00000u;
    if ((sec & 3) != 2 || nsec == 2) return -1;              /* only section descriptors */
    u32 ap = sec >> 10 & 3, *t = l2[nsec];
    for (u32 i = 0; i < 256; i++) {
        u32 a = mb + (i << 12);
        t[i] = a | ap * 0x55u << 4 | (a < hi ? 0xc : (sec & 0xc)) | 2;       /* a small page; AP the same in its 4 parts */
    }
    dclean_inval((u32)t, (u32)t + sizeof l2[0]);             /* the table walk reads RAM */
    sec_saved[nsec] = sec; sec_mb[nsec] = mb; nsec++;
    *l1 = (u32)t | (sec & 0x1e0) | 0x10 | 1;                 /* a coarse table, the section's domain */
    tlb_inval();
    return 0;
}
void mmu_uncache_all(void) {
    while (nsec) {
        nsec--;
        dclean_inval(sec_mb[nsec], sec_mb[nsec] + 0x100000);
        *(volatile u32 *)(ttb() + (sec_mb[nsec] >> 20) * 4) = sec_saved[nsec];
        tlb_inval();
        dclean_inval(sec_mb[nsec], sec_mb[nsec] + 0x100000);
    }
    mmu_uncache();
}
#endif
