/* The pad's RAM on the wup-homebrew kernel: 4 MiB, all of it ours (Nintendo's firmware never runs once the kernel
 * has started). One map, used by the kernel (kernel/src/ram.h) and by libwup.
 *
 *   0x000000  the kernel: vectors (0x80: its table for apps, wupk.h), code, data, stacks, its heap
 *   0x07f000  the hand-off block between the menu and the apps it starts (handoff.h), 4 KB
 *   0x080000  THE APP: one flat block. Its image (code, constants, data) at the start, linked here; then its .bss;
 *             then its heap (malloc) up to WUP_APP_END. About 1.8 MB
 *   0x253000  the sound ring (45 KB), the microphone ring (64 KB)
 *   0x26fb00  screen 0, RGB565 854 x 480 (820 KB): what the LCD's UI layer shows
 *   0x337d80  screen 1 (820 KB, to the end of RAM). Also, between apps, where the next app's stored image waits
 *             (WUP_STAGE); and the 8-bit paletted screen (wup_screen8) for apps that use that instead
 *
 * 0x200000-0x400000 is mapped write-through (the kernel's mmu.c): what DMA reads (screens, the sound ring) sees every
 * write. The video decoder and the camera's encoder, while they run, use the screens' memory (as in wup-os). */
#pragma once

#define WUP_KERNEL_END      0x0007f000u
#define WUP_HANDOFF_ADDR    0x0007f000u
#define WUP_APP_BASE        0x00080000u
#define WUP_APP_END         0x00253000u
#define WUP_SOUND_RING      0x00253000u      /* 0xB400 bytes: 11520 stereo 16-bit frames, 0.24 s at 48 kHz */
#define WUP_SOUND_RING_SIZE 0x0000b400u
#define WUP_MIC_RING        0x0025e800u      /* 64 KB */
#define WUP_MIC_RING_SIZE   0x00010000u
#define WUP_FB0             0x0026fb00u
#define WUP_FB1             0x00337d80u
#define WUP_FB_SIZE         (854u * 480u * 2u)
#define WUP_STAGE           WUP_FB1          /* a stored (deflated) app image between apps: up to WUP_FB_SIZE */
#define WUP_STAGE_MAX       WUP_FB_SIZE
#define WUP_FB8             WUP_FB1          /* the 8-bit paletted screen (854 x 480 bytes) */
