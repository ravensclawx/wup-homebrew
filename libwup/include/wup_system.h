/* For system apps only (app.cfg: system = yes: the Homebrew Menu, Software Update). Ordinary apps do not get these.
 *
 * The menu slot: a new Homebrew Menu, written the safe way (as wup-menu's tools/menu_install.py does). Its format:
 * a 20-byte header {"WMNU", stored size, crc32 of the stored image, inflated size, stamp} at the start of a 4 KB
 * header sector, then the deflated menu image (wup-menu: tools/mkflash.py menu_header). Between begin and end the
 * slot holds no valid menu: the loader then runs the firmware's own copy (the Wii U install's bundle). */
#pragma once
#include <stdint.h>

int wup_menu_slot_begin(const uint8_t header[20]); /* 0: it fits the slot, the slot's header is erased */
int wup_menu_slot_write(const void *data, uint32_t n);   /* the stored image, in order, in any pieces; 0 or < 0 */
int wup_menu_slot_end(void);                       /* 0: all of it arrived and its CRC matches: the header is written */
/* On the wup-homebrew kernel (WUP_KERNEL) the menu slot is the kernel's menu (0xB2000), and the kernel itself has a
   slot (0x90000, header {"GPST", size, crc32}): begun with this, then the same write and end calls. Without a whole
   kernel (or a whole menu) A at power-on starts stock (wup-menu boot/early.c). */
int wup_kernel_slot_begin(const uint8_t header[20]);
