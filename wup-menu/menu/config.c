/* config: the menu's settings in the SPI flash (through the kernel). Two 4 KB sectors (0xB0000, 0xB1000), below both
 * firmware banks, are used alternately: each save goes to the sector not holding the newest record, with seq+1 and a
 * CRC, so a power loss mid-save keeps the previous record. The early hook reads the Autoboot byte from here
 * (wup-menu/boot/early.c). */
#include "wupk.h"
#include "config.h"
#include "armcpu.h"
typedef unsigned u32; typedef unsigned char u8;
#define SECT0 0x000b0000u
#define SECT1 0x000b1000u
#define MAGIC 0x47464357u                       /* "WCFG" */

typedef struct { u32 magic, version, seq, len, crc; config_t c; } rec_t;
_Static_assert(sizeof(config_t) == 76, "the record's size is fixed: the early hook (boot/early.c) checks it");

static void cp(void *d, const void *s, u32 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static int read_rec(u32 addr, rec_t *r) {
    if (WUPK->flash_read(addr, r, sizeof *r)) return 0;
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
    return WUPK->flash_erase4k(SECT0) | WUPK->flash_erase4k(SECT1);
}
int config_save(const config_t *c) {
    rec_t r; r.magic = MAGIC; r.version = 1; r.seq = newest_seq + 1; r.len = sizeof r.c; cp(&r.c, c, sizeof *c);
    r.crc = crc32((const u8 *)&r.c, sizeof r.c);
    u32 dst = newest_addr == SECT0 ? SECT1 : SECT0;
    if (WUPK->flash_erase4k(dst)) return -1;
    if (WUPK->flash_write(dst, &r, sizeof r)) return -2;
    rec_t back;
    if (!read_rec(dst, &back) || back.seq != r.seq) return -3;
    newest_seq = r.seq; newest_addr = dst;
    return 0;
}
