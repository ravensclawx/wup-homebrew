/* For system apps only (app.cfg: system = yes: the Homebrew Menu, Software Update). Ordinary apps do not get these.
 *
 * The two slots Software Update writes, each the safe way: its header erased first and written last, once the whole
 * image has arrived and its CRC matches. Without a whole kernel or a whole menu, A at power-on starts stock (the early
 * hook checks both: wup-menu/boot/early.c), and Software Update can run again from there.
 *   the menu (0xB2000): a 20-byte header {"WMNU", stored size, crc32 of the stored image, inflated size, stamp} at the
 *     start of a 4 KB header sector, then the deflated menu image (wup-menu: tools/mkflash.py kernel_menu_slot)
 *   the kernel (0x90000): header {"GPST", size, crc32}, then the kernel (tools/mkflash.py system_slot) */
#pragma once
#include <stdint.h>

int wup_menu_slot_begin(const uint8_t header[20]); /* 0: it fits the slot, the slot's header is erased */
int wup_menu_slot_write(const void *data, uint32_t n);   /* the stored image, in order, in any pieces; 0 or < 0 */
int wup_menu_slot_end(void);                       /* 0: all of it arrived and its CRC matches: the header is written */
int wup_kernel_slot_begin(const uint8_t header[20]);   /* the kernel's slot: then the same write and end calls */
