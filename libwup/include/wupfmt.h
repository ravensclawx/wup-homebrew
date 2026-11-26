/* .wup: a GamePad app file (like a Switch NRO). Built by tools/wupbuild.py (packed by tools/wupack.py).
 *   header (little endian), then the code (an app image: HBRW header, linked for WUP_APP_BASE; deflated), then the
 *   icon (WUP_ICON x WUP_ICON RGB565 rows, optional: the menu draws a placeholder without one).
 * The menu lists /apps/<x>.wup and /apps/<dir>/<dir>.wup; an app is started by staging its code for the kernel
 * (libwup's wup_exec, include/handoff.h). */
#define HB_MAGIC 0x57524248u               /* "HBRW": an app image */
#pragma once
#include <stdint.h>
#define WUP_MAGIC 0x30505557u              /* "WUP0" */
#define WUP_ICON  64                       /* icon size in pixels (square) */
typedef struct {
    uint32_t magic, hdr_size;
    uint32_t code_off, code_size, code_crc;
    uint32_t icon_off, icon_w, icon_h;     /* icon_off 0: no icon */
    char name[32], author[32], version[16];
    char description[96];                  /* only if hdr_size >= WUP_HDR_V2 */
    uint32_t code_raw, flags;              /* only if hdr_size >= WUP_HDR_V3: the code is deflated (raw deflate), this is
                                              its inflated size; code_size and code_crc are of the stored bytes */
    uint32_t reserved[5];                  /* (hdr_size >= WUP_HDR_V4: v1's code for big memory; 0 now) */
} wup_hdr_t;
#define WUP_HDR_V1 112                     /* header size before the description existed */
#define WUP_FLAG_KERNEL 0x1u               /* flags: built for the wup-homebrew kernel. v1 apps (for Nintendo's
                                              firmware) lack it and are not run (libwup wup_exec) */
#define WUP_HDR_V2 208
#define WUP_HDR_V3 216
#define WUP_HDR_V4 236

/* the app image's own header (HBRW, first in the image, at WUP_APP_BASE: wup.h WUP_APP): entry, the image's size
   (code, constants, data: its .bss follows), the app's name */
typedef struct { uint32_t magic, entry, size; char name[32]; uint32_t reserved[3]; } hb_header;
