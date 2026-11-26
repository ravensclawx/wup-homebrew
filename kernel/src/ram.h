/* The RAM map: libwup/include/wup_ram.h, shared with libwup (the kernel below 0x7F000, then the hand-off block, the
 * app's one flat block, the sound and microphone rings, the two screens). */
#pragma once
#include "types.h"
#define RAM_SIZE   0x400000u
#include "../../libwup/include/wup_ram.h"
#define KERNEL_END WUP_KERNEL_END
#define FB0_ADDR   WUP_FB0
#define SCREEN_W   854
#define SCREEN_H   480
extern char _image_end[], _bss_start[], _bss_end[], _ttb[], _heap_start[];
#define AUDIO_RING WUP_SOUND_RING              /* (shared with libwup's sound: the same ring, the same size) */
#define MIC_RING   WUP_MIC_RING
