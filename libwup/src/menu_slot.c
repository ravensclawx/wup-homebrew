/* menu_slot: a new Homebrew Menu or kernel into its flash slot (include/wup_system.h; system apps only). Each 4 KB
 * sector is erased, written and read back; the header goes last. */
#include "wupk.h"
#include <string.h>
#include "wup_system.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8;
/* the slots (wup-menu tools/mkflash.py): the menu's at 0xB2000 and the kernel's at 0x90000 (the early hook checks
   both) */
#define MENU_HDR_ 0x000b2000u
#define MENU_END_ 0x00100000u
#define KERN_HDR_ 0x00090000u
#define KERN_END_ 0x000b0000u

static u8 hdr[20], sec[4096];
static u32 at, fill, crc, size, open_, SLOT_HDR, SLOT_IMG, SLOT_END;

static u8 back[4096];
static int sector(u32 addr, const u8 *d, u32 n) {         /* erased, written, read back */
    if (WUPK->flash_erase4k(addr)) return -1;
    if (!n) return 0;
    if (WUPK->flash_write(addr, d, n)) return -1;
    return WUPK->flash_read(addr, back, n) || memcmp(back, d, n) ? -1 : 0;
}
static int slot_begin(const uint8_t header[20], u32 magic, u32 at_hdr, u32 end) {
    u32 h[5]; memcpy(h, header, 20);
    SLOT_HDR = at_hdr; SLOT_IMG = at_hdr + 0x1000; SLOT_END = end;
    if (h[0] != magic || h[1] < 64 || SLOT_IMG + h[1] > SLOT_END) return -1;   /* the magic, and it fits */
    memcpy(hdr, header, 20); size = h[1]; at = fill = crc = 0; open_ = 1;
    return sector(SLOT_HDR, 0, 0);                    /* the old menu's header goes first */
}
int wup_menu_slot_begin(const uint8_t header[20]) { return slot_begin(header, 0x554e4d57u, MENU_HDR_, MENU_END_); }   /* "WMNU" */
int wup_kernel_slot_begin(const uint8_t header[20]) { return slot_begin(header, 0x54535047u, KERN_HDR_, KERN_END_); } /* "GPST" */
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
