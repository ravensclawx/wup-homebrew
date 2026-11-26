/* The Homebrew Menu and its apps, one after another (the hand-off block: libwup/include/handoff.h). The menu comes
 * from the kernel's menu slot (0x10000: header {"WMNU", stored size, crc32, inflated size, stamp}, image 0x11000);
 * images are app images (wupfmt.h), usually deflated (tinf), inflated straight into the app block (wup_ram.h). An app
 * that returns WUP_HANDOFF_RUN has staged the next one; after any other the menu runs again, also after the menu
 * itself ends: the way out is holding Power (the choice is at power-on). */
#include "os.h"
#include "../../libwup/include/handoff.h"
#include "../../libwup/include/wupfmt.h"
#include "../../libwup/third_party/tinf/src/tinf.h"

#define MENU_HDR   0x00010000u              /* the menu's slot (wup-menu/tools/mkflash.py) */
#define MENU_END   0x000b0000u
#define WMNU       0x554e4d57u

typedef struct { u32 hdr, end; u32 h[5]; } menu_at;
static int menu_header(menu_at *m) {
    if (flash_read(m->hdr, m->h, sizeof m->h)) return 0;
    u32 size = m->h[1];
    if (m->h[0] != WMNU || size < 64 || size > m->end - (m->hdr + 0x1000)) return 0;
    if (m->h[4] == 0xffffffffu) m->h[4] = 0;
    return 1;
}
static int menu_load(volatile wup_handoff_t *pb, const menu_at *m) {
    u32 size = m->h[1];
    if (size > WUP_HANDOFF_STAGE_MAX || flash_read(m->hdr + 0x1000, (void *)WUP_HANDOFF_STAGE, size)) { klog("apps: flash read failed\n"); return 0; }
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, size) != m->h[2]) { klog("apps: menu CRC mismatch\n"); return 0; }
    pb->stage_size = size; pb->stage_raw = m->h[3];
    return 1;
}
static int stage_menu(volatile wup_handoff_t *pb) {
    static menu_at m = { MENU_HDR, MENU_END, { 0 } };
    if (!menu_header(&m)) { klog("apps: no menu in flash\n"); return 0; }
    return menu_load(pb, &m);
}

static void sync_code(u32 a, u32 n) {           /* the written code out of the D-cache, the I-cache dropped */
    dcache_clean((void *)a, n);
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

/* run the staged image; < 0: it is not one. An app image is one piece, linked at WUP_APP_BASE (libwup's wupbuild.py):
   its HBRW header first, then code, constants and data; its .bss and heap follow it up to WUP_APP_END */
static int run_staged(volatile wup_handoff_t *pb) {
    const u8 *img = (const u8 *)WUP_HANDOFF_STAGE;
    u32 len = pb->stage_size;
    u8 *app = (u8 *)WUP_APP_BASE;
    if (len > WUP_HANDOFF_STAGE_MAX) { klog("apps: staged image too big\n"); return -1; }
    if (pb->stage_raw) {
        unsigned n = WUP_HANDOFF_WORK_MAX;
        if (pb->stage_raw > n || tinf_uncompress(app, &n, img, len) != TINF_OK || n != pb->stage_raw) { klog("apps: inflate failed\n"); return -1; }
        len = n;
    } else memmove(app, img, len);
    const hb_header *h = (const hb_header *)app;
    if (len < sizeof *h || h->magic != HB_MAGIC) { klog("apps: not an app image\n"); return -1; }
    if (h->entry < WUP_APP_BASE || h->entry >= WUP_APP_BASE + len) { klog("apps: bad app header\n"); return -1; }
    u32 entry = h->entry;
    klog("apps: running %s (%u KB)\n", (const char *)h->name, len >> 10);
    sync_code(WUP_APP_BASE, len);
    return ((int (*)(void))entry)();
}

void apps_run(void *unused) {
    (void)unused;
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    memset((void *)pb, 0, sizeof *pb);
    pb->magic = WUP_HANDOFF_MAGIC; pb->version = 2;
    for (int staged = 0;;) {
        if (!staged && !stage_menu(pb)) break;
        int menu = !staged;
        int r = run_staged(pb);
        rumble(0); mic_close(); audio_stop(); video_off();   /* (whatever the app left on) */
        if (r < 0 && menu) break;
        if (!menu) pb->runs++;
        staged = r == WUP_HANDOFF_RUN;
        if (menu && !staged) klog("apps: the menu ended: again\n");   /* (the way out is holding Power) */
    }
    klog("apps: no menu to run\n");
    for (;;) sched_sleep(1000);
}
