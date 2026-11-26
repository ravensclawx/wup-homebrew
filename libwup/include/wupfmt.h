/* .wup: a GamePad app file (like a Switch NRO). Built by tools/wupbuild.py (packed by tools/wupack.py).
 *   header (little endian), then the code (an app image: HBRW header, linked for 0x350000; deflated), then the
 *   icon (WUP_ICON x WUP_ICON RGB565 rows, optional: the menu draws a placeholder without one).
 * The menu lists /apps/<x>.wup and /apps/<dir>/<dir>.wup; an app is started by staging its code for the loader
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
    uint32_t big_addr, big_off, big_size, big_raw, big_crc;   /* only if hdr_size >= WUP_HDR_V4 and big_size: code that did not
                                              fit the two segments, raw-deflated; libwup puts it at big_addr (WUP_BIGMEM)
                                              before main() (src/big.c). The loader never sees it. */
} wup_hdr_t;
#define WUP_HDR_V1 112                     /* header size before the description existed */
#define WUP_FLAG_KERNEL 0x1u               /* flags: built for the wup-homebrew kernel (WUP_KERNEL=1). Apps built for
                                              Nintendo's firmware lack it; each side runs only its own (libwup wup_exec) */
#define WUP_HDR_V2 208
#define WUP_HDR_V3 216
#define WUP_HDR_V4 236

/* the app image's own header (HBRW, at 0x350000; firmware.h hb_header). Large apps (libwup, wupbuild.py)
   go on with a second segment: it follows the first 'size' bytes of the image and belongs at seg2_addr,
   inside WUP_SPARE (0x0DA000..0x136000). The menu places both; libwup's entry (in the first segment)
   syncs the caches before running code from the second. */
#define HB_SEG2_MAGIC  0x32474553u   /* "SEG2" */
#define HB_SEG2_START  0x000da000u
#define HB_SEG2_END    0x00136000u
typedef struct { uint32_t magic, entry, size; char name[32]; uint32_t seg2_magic, seg2_addr, seg2_size; } hb_header2;
