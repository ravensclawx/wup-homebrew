/* config: the menu's settings in SPI flash, through the stock firmware's flash driver (LVC rev 7677):
 *   read(h, addr, dst, len, &done) 0x9140   write(h, addr, src, len, &done) 0x8fb2
 *   erase_sectors(h, first 4 KB index, count) 0x92b8   (erase_blocks 64 KB: 0x93b4)
 *   h = *(u32 *)0xbb5f4; flash = Micron N25Q256 (20 BA 19), 8192 x 4 KB, page 256.
 * Verified on the real pad 2026-11-15 (erase / write / read back one sector at 0x8F0000).
 * Two 4 KB sectors (0xB0000, 0xB1000), below both firmware banks (docs/ARCHITECTURE.md), are used alternately: each save
 * goes to the sector not holding the newest record, with seq+1 and a CRC, so a power loss mid-save
 * keeps the previous record. The driver DMAs, so data passes through an uncached buffer (0x34E000). */
#include "wup_flash.h"
#include "config.h"
#include "armcpu.h"
typedef unsigned u32; typedef unsigned char u8;
#define SECT0 0x000b0000u
#define SECT1 0x000b1000u
#define BUF ((u8 *)0x0034e000u)                 /* uncached, DMA-reachable: libwup's flash bounce buffer (handoff.h) */
#define MAGIC 0x47464357u                       /* "WCFG" */

typedef struct { u32 magic, version, seq, len, crc; config_t c; } rec_t;
_Static_assert(sizeof(config_t) == 76, "the record's size is fixed: the loader (boot/loader.c) checks it");

static void cp(void *d, const void *s, u32 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static int read_rec(u32 addr, rec_t *r) {
    u32 done = 0;
    if (FL_READ(HANDLE, addr, BUF, sizeof *r, &done)) return 0;
    cp(r, BUF, sizeof *r);
    return r->magic == MAGIC && r->version == 1 && r->len == sizeof r->c && r->crc == crc32((const u8 *)&r->c, sizeof r->c);
}
static u32 newest_seq, newest_addr;
int config_load(config_t *c) {
    rec_t a, b; int va = read_rec(SECT0, &a), vb = read_rec(SECT1, &b);
    newest_seq = 0; newest_addr = 0;
    if (!va && !vb) return 0;
    rec_t *n = !vb || (va && a.seq >= b.seq) ? &a : &b;
    newest_seq = n->seq; newest_addr = n == &a ? SECT0 : SECT1;
    cp(c, &n->c, sizeof *c);
    return 1;
}
int config_erase(void) {
    newest_seq = 0; newest_addr = 0;
    return FL_ERASE4K(HANDLE, SECT0 >> 12, 1) | FL_ERASE4K(HANDLE, SECT1 >> 12, 1);
}
int config_save(const config_t *c) {
    rec_t r; r.magic = MAGIC; r.version = 1; r.seq = newest_seq + 1; r.len = sizeof r.c; cp(&r.c, c, sizeof *c);
    r.crc = crc32((const u8 *)&r.c, sizeof r.c);
    u32 dst = newest_addr == SECT0 ? SECT1 : SECT0, done = 0;
    if (FL_ERASE4K(HANDLE, dst >> 12, 1)) return -1;
    cp(BUF, &r, sizeof r);
    if (FL_WRITE(HANDLE, dst, BUF, sizeof r, &done)) return -2;
    rec_t back;
    if (!read_rec(dst, &back) || back.seq != r.seq) return -3;
    newest_seq = r.seq; newest_addr = dst;
    return 0;
}
