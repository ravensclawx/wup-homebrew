/* The RAM map (4 MiB; Nintendo's firmware is gone once the kernel runs).
 *   0x000000  vectors; 0x80: the kernel's table pointer for apps (WUPK_PTR, libwup's wupk.h)
 *   0x010000  the kernel: image, .bss, page table, stacks, then its heap, below WUP_SPARE (0xDA000)
 *   0x0DA000  apps, as libwup has always laid them out (wupbuild.py): WUP_SPARE 0xDA000-0x136000, WUP_BIGMEM
 *             0x190000-0x2FA400 (the RGB565 screen at 0x221400 in it), the app 0x350000-0x380000, the hand-off block
 *             0x34F000, the 8-bit screen 0x388400. Not yet used: 0x136000-0x190000, 0x2FA400-0x340000, above 0x3EE000
 *   0x200000-0x400000 mapped write-through (mmu.c): what DMA reads (the screens, the sound ring) gets every write */
#pragma once
#include "types.h"
#define RAM_SIZE   0x400000u
#define KERNEL_END 0x0da000u
#define FB0_ADDR   0x221400u                    /* (libwup's RGB565 screen) */
#define SCREEN_W   854
#define SCREEN_H   480
extern char _image_end[], _bss_start[], _bss_end[], _ttb[], _heap_start[];
#define AUDIO_RING 0x300000u                    /* (the kernel's own sound ring and microphone ring: free memory) */
#define MIC_RING   0x30c000u
