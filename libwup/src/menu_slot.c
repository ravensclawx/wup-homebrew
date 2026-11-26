/* menu_slot: a new Homebrew Menu into the menu slot (include/wup_system.h; system apps only). The slot is wup-menu's
 * (tools/mkflash.py, boot/loader.c): header sector 0x30000, image 0x31000 up to 0x8FFFF. Each 4 KB sector is erased,
 * written and read back through the firmware's flash driver; the header goes last. */
#include "wup_flash.h"
#include <string.h>
#include "wup_system.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8;
#define BUF ((u8 *)0x0034e000u)                       /* uncached, DMA-reachable: the flash bounce buffer (handoff.h) */
#define SLOT_HDR 0x00030000u
#define SLOT_IMG 0x00031000u
#define SLOT_END 0x00090000u

static u8 hdr[20], sec[4096];
static u32 at, fill, crc, size, open_;

static int sector(u32 addr, const u8 *d, u32 n) {
    u32 done;
    if (FL_ERASE4K(HANDLE, addr >> 12, 1)) return -1;
    if (!n) return 0;
    memcpy(BUF, d, n);
    if (FL_WRITE(HANDLE, addr, BUF, n, &done)) return -1;
    memset(BUF, 0, n);
    return FL_READ(HANDLE, addr, BUF, n, &done) || memcmp(BUF, d, n) ? -1 : 0;
}
int wup_menu_slot_begin(const uint8_t header[20]) {
    u32 h[5]; memcpy(h, header, 20);
    if (h[0] != 0x554e4d57u || h[1] < 64 || SLOT_IMG + h[1] > SLOT_END) return -1;   /* "WMNU", and it fits */
    memcpy(hdr, header, 20); size = h[1]; at = fill = crc = 0; open_ = 1;
    return sector(SLOT_HDR, 0, 0);                    /* the old menu's header goes first */
}
int wup_menu_slot_write(const void *data, uint32_t n) {
    const u8 *d = data;
    if (!open_ || at + fill + n > size) return -1;
    crc = crc32_update(crc, d, n);
    while (n) {
        u32 k = n < 4096 - fill ? n : 4096 - fill;
        memcpy(sec + fill, d, k); fill += k; d += k; n -= k;
        if (fill == 4096) { if (sector(SLOT_IMG + at, sec, 4096)) return -2; at += 4096; fill = 0; }
    }
    return 0;
}
int wup_menu_slot_end(void) {
    if (!open_) return -1;
    open_ = 0;
    if (fill && sector(SLOT_IMG + at, sec, fill)) return -2;
    u32 h[5]; memcpy(h, hdr, 20);
    if (at + fill != size || crc != h[2]) return -3;  /* (incomplete, or not what the header says) */
    static u8 page[4096];
    memset(page, 0xff, sizeof page); memcpy(page, hdr, 20);
    return sector(SLOT_HDR, page, 4096) ? -2 : 0;     /* the new menu is valid from here on */
}
