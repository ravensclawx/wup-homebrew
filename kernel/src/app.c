/* The Homebrew Menu and its apps, one after another, as the loader ran them on Nintendo's firmware (wup-menu
 * boot/loader.c; the same images, the same hand-off block: libwup/include/handoff.h). The menu comes from its flash
 * slot (0x30000: header {"WMNU", stored size, crc32, inflated size, stamp}, image 0x31000) or from the bundle in the
 * running bank (bank + BUNDLE_OFF + 0x2000: the Wii U install), the newer one; images are app images (wupfmt.h),
 * usually deflated (tinf). An app that returns WUP_HANDOFF_RUN has staged the next one; after any other the menu is
 * loaded again. When the menu itself ends, the pad starts Nintendo's firmware again (stock). */
#include "os.h"
#include "../../libwup/include/handoff.h"
#include "../../libwup/include/wupfmt.h"
#include "../../libwup/third_party/tinf/src/tinf.h"

#define MENU_HDR   0x000b2000u              /* the menu built for this kernel (WUP_KERNEL=1; mkflash.py --kernel-menu). Not
                                                the menu slot at 0x30000: that one is built for Nintendo's firmware (A) */
#define MENU_END   0x00100000u
#define BUNDLE_OFF 0x002c9000u                  /* (wup-menu tools/mkflash.py) */
#define BANK_SIZE  0x00400000u
#define WMNU       0x554e4d57u
#define APP_LO     0x00350000u
#define APP_HI     0x00380000u

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
    if (flash_read(m->hdr + 0x1000, (void *)WUP_HANDOFF_STAGE, size)) { klog("apps: flash read failed\n"); return 0; }
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, size) != m->h[2]) { klog("apps: menu CRC mismatch\n"); return 0; }
    pb->stage_size = size; pb->stage_raw = m->h[3];
    return 1;
}
static int stage_menu(volatile wup_handoff_t *pb) {
    static menu_at m[2];
    u8 sel = 0;
    flash_read(0xf000, &sel, 1);
    u32 bank = sel == 1 ? 0x500000u : 0x100000u;
    m[0].hdr = bank + BUNDLE_OFF + 0x2000; m[0].end = bank + BANK_SIZE;
    m[1].hdr = MENU_HDR;                   m[1].end = MENU_END;
    int ok0 = 0, ok1 = menu_header(&m[1]);              /* (not the bundle: its menu is for Nintendo's firmware) */
    (void)menu_header;
    if (!ok0 && !ok1) { klog("apps: no menu in flash\n"); return 0; }
    int first = ok0 && (!ok1 || m[0].h[4] >= m[1].h[4]) ? 0 : 1;
    if ((first ? ok1 : ok0) && menu_load(pb, &m[first])) return 1;
    return (first ? ok0 : ok1) && menu_load(pb, &m[!first]);
}

static void sync_code(u32 a, u32 n) {           /* the written code out of the D-cache, the I-cache dropped */
    dcache_clean((void *)a, n);
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

/* run the staged image; < 0: it is not one */
static int run_staged(volatile wup_handoff_t *pb) {
    const u8 *img = (const u8 *)WUP_HANDOFF_STAGE;
    u32 len = pb->stage_size;
    if (pb->stage_raw) {
        unsigned n = WUP_HANDOFF_WORK_MAX;
        if (pb->stage_raw > n || tinf_uncompress((void *)WUP_HANDOFF_WORK, &n, img, len) != TINF_OK || n != pb->stage_raw) { klog("apps: inflate failed\n"); return -1; }
        img = (const u8 *)WUP_HANDOFF_WORK; len = n;
    }
    const hb_header2 *h = (const hb_header2 *)img;
    u32 n1 = len, n2 = 0;
    if (len < sizeof *h || h->magic != HB_MAGIC) { klog("apps: not an app image\n"); return -1; }
    if (h->seg2_magic == HB_SEG2_MAGIC) {
        n1 = h->size; n2 = h->seg2_size;
        if (n1 > len || n1 + n2 != len || h->seg2_addr < HB_SEG2_START || h->seg2_addr + n2 > HB_SEG2_END) { klog("apps: bad second segment\n"); return -1; }
    }
    if (n1 > APP_HI - APP_LO || h->entry < APP_LO || h->entry >= APP_LO + n1) { klog("apps: bad app header\n"); return -1; }
    u32 a2 = h->seg2_addr, entry = h->entry;
    klog("apps: running %s\n", (const char *)h->name);
    if (n2) { memmove((void *)a2, img + n1, n2); sync_code(a2, n2); }   /* (the second segment first: WORK may overlap it) */
    memmove((void *)APP_LO, img, n1); sync_code(APP_LO, n1);
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
        if (r < 0 && menu) break;
        if (!menu) pb->runs++;
        staged = r == WUP_HANDOFF_RUN;
        if (menu && !staged) { klog("apps: the menu ended\n"); break; }
    }
    klog("apps: Nintendo's start again\n");
    sys_reboot();
    for (;;) sched_sleep(1000);
}
