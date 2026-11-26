/* on-pad helper for tools/menu_install.py: program one 4 KB flash sector from RAM through the stock
 * firmware's driver. args (at the address passed): flash addr (4 KB aligned), source RAM (uncached,
 * DMA-reachable), length (<= 4096), a read-back buffer (uncached, 1 KB). Erases the sector, writes, reads back and
 * compares. On the wup-homebrew kernel (its service table at 0x80, libwup/include/wupk.h) through the kernel's flash
 * calls instead. */
typedef unsigned u32; typedef unsigned char u8;
typedef int (*rw_fn)(u32 h, u32 addr, void *buf, u32 len, u32 *done);
typedef int (*er_fn)(u32 h, u32 first, u32 count);
#define FL_READ  ((rw_fn)0x00009141)
#define FL_WRITE ((rw_fn)0x00008fb3)
#define FL_ERASE4K ((er_fn)0x000092b9)
#define HANDLE (*(volatile u32 *)0x000bb5f4)
typedef struct { u32 magic, version; void *f[18]; int (*read)(u32, void *, u32); int (*write)(u32, const void *, u32);
                 int (*erase4k)(u32); } wupk_head;       /* (wupk.h: flash_read is entry 20 of the table) */
__attribute__((section(".text.entry"))) int flashwr(const u32 *a) {
    u32 fa = a[0], len = a[2], done = 0; const u8 *src = (const u8 *)a[1]; u8 *CHECK = (u8 *)a[3];
    if ((fa & 0xfff) || len == 0 || len > 4096) return -1;
    const wupk_head *k = *(const wupk_head *const *)0x80;
    if (k && (u32)k < 0x400000u && k->magic == 0x4b505557u) {
        if (k->erase4k(fa)) return -2;
        if (k->write(fa, src, len)) return -3;
        for (u32 o = 0; o < len; o += 1024) {
            u32 n = len - o < 1024 ? len - o : 1024;
            if (k->read(fa + o, CHECK, n)) return -4;
            for (u32 i = 0; i < n; i++) if (CHECK[i] != src[o + i]) return -5;
        }
        return 0;
    }
    if (FL_ERASE4K(HANDLE, fa >> 12, 1)) return -2;
    if (FL_WRITE(HANDLE, fa, (void *)src, len, &done)) return -3;
    for (u32 o = 0; o < len; o += 1024) {
        u32 k = len - o < 1024 ? len - o : 1024;
        if (FL_READ(HANDLE, fa + o, CHECK, k, &done)) return -4;
        for (u32 i = 0; i < k; i++) if (CHECK[i] != src[o + i]) return -5;
    }
    return 0;
}
