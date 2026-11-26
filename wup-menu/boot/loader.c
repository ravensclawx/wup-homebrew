/* The loader, run at RAM 0x340000 ("CFW1" header). The boot hook (hook.S) loads it and calls it on every normal boot,
 * from the bundle in the running firmware bank (the Wii U install) or from flash 0x10000 (the UART install). It decides whether homebrew runs:
 *   X held at power-on, a system in its slot  -> that system takes the whole pad (takeover below)
 *   A held at power-on                       -> the menu
 *   Autoboot set (the menu's settings)       -> the menu
 *   otherwise                                -> return at once: the stock boot
 * and then runs app images, one after another (libwup/include/handoff.h): the menu, and whatever an app stages before it
 * returns WUP_HANDOFF_RUN (the menu stages the app to start; libwup's wup_exec). After any other return the menu is
 * loaded again. The menu is a header sector {"WMNU", stored size, crc32, inflated size, stamp} and the image after it,
 * in two places: the menu slot (0x30000, image 0x31000: updates over Wi-Fi) and the bundle in the running bank
 * (bank + BUNDLE_OFF + 0x2000 / 0x3000: the Wii U install). The newer stamp runs (a missing stamp is the oldest); if
 * that one is damaged, the other. How large the menu slot is, only the tools and the filesystem's layout say (tools/mkflash.py,
 * libwup/src/fs.c): the loader takes the header's size, up to the end of our flash gap. Images are app images (libwup/include/wupfmt.h), usually deflated: inflated with tinf.
 * On any problem with the menu the loader returns, and the pad boots the stock firmware. */
#include <stddef.h>
#include "firmware.h"
#include "armcpu.h"
#include "handoff.h"
#include "wupfmt.h"
#include "tinf.h"

void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) {
    u8 *p = d; const u8 *q = s;
    if (!(((u32)p | (u32)q) & 3)) for (; n >= 4; n -= 4, p += 4, q += 4) *(u32 *)p = *(const u32 *)q;
    while (n--) *p++ = *q++;
    return d;
}

#define MENU_HDR   0x00030000u            /* the menu slot: its header sector, then the image */
#define MENU_IMG   0x00031000u
#define MENU_MAX   (0x00100000u - MENU_IMG)  /* (no further than the gap; the CRC says whether it is a menu) */
#define SELECTOR   0x0000f000u            /* the bank selector byte: 1 boots Product2 */
#define BANK_SIZE  0x00400000u
#ifndef BUNDLE_OFF
#error BUNDLE_OFF (tools/mkflash.py)
#endif
#define WMNU       0x554e4d57u            /* "WMNU" */
#define CFG0       0x000b0000u
#define CFG1       0x000b1000u
#define APP_LO     0x00350000u            /* an image's first segment */
#define APP_HI     0x00380000u

/* the menu's config record (menu/config.c): only the autoboot byte is needed here */
typedef struct { u32 magic, version, seq, len, crc; u8 c[76]; } rec_t;
static int cfg_autoboot(void) {
    static rec_t r[2];
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (flash_read(i ? CFG1 : CFG0, &r[i], sizeof r[i])) continue;
        if (r[i].magic != 0x47464357u || r[i].version != 1 || r[i].len != 76 || r[i].crc != crc32(r[i].c, 76)) continue;
        if (best < 0 || r[i].seq > r[best].seq) best = i;
    }
    return best >= 0 ? r[best].c[1] : 0;                /* config_t: light, autoboot, ... */
}

/* where a menu is: its header sector, and how far its image may reach */
typedef struct { u32 hdr, end; u32 h[5]; } menu_at;
static int menu_header(menu_at *m) {
    if (flash_read(m->hdr, m->h, sizeof m->h)) return 0;
    u32 size = m->h[1];
    if (m->h[0] != WMNU || size < 64 || size > m->end - (m->hdr + 0x1000)) return 0;
    if (m->h[4] == 0xffffffffu) m->h[4] = 0;            /* (no stamp: written before there were stamps) */
    return 1;
}
static int menu_load(volatile wup_handoff_t *pb, const menu_at *m) {
    u32 size = m->h[1];
    if (flash_read(m->hdr + 0x1000, (void *)WUP_HANDOFF_STAGE, size)) { dbg("[loader] flash read failed\n"); return 0; }
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, size) != m->h[2]) { dbg("[loader] menu CRC mismatch\n"); return 0; }
    pb->stage_size = size; pb->stage_raw = m->h[3];
    return 1;
}
/* the menu to the stage, checked; 0 = there is none */
static int stage_menu(volatile wup_handoff_t *pb) {
    static menu_at m[2];
    static u8 sel[4];
    u32 bank = !flash_read(SELECTOR, sel, sizeof sel) && sel[0] == 1 ? 0x00500000u : 0x00100000u;
    m[0].hdr = bank + BUNDLE_OFF + 0x2000; m[0].end = bank + BANK_SIZE;
    m[1].hdr = MENU_HDR;                   m[1].end = MENU_HDR + 0x1000 + MENU_MAX;
    int ok0 = menu_header(&m[0]), ok1 = menu_header(&m[1]);
    int first = ok0 && (!ok1 || m[0].h[4] >= m[1].h[4]) ? 0 : 1;
    if (!ok0 && !ok1) { dbg("[loader] no menu in flash\n"); return 0; }
    if ((first ? ok1 : ok0) && menu_load(pb, &m[first])) return 1;
    return (first ? ok0 : ok1) && menu_load(pb, &m[!first]);
}
/* run the staged image; < 0: it is not one */
static int run_staged(volatile wup_handoff_t *pb) {
    const u8 *img = (const u8 *)WUP_HANDOFF_STAGE;
    u32 len = pb->stage_size;
    if (pb->stage_raw) {                                /* deflated: inflate it into the big memory */
        unsigned n = WUP_HANDOFF_WORK_MAX;
        if (pb->stage_raw > n || tinf_uncompress((void *)WUP_HANDOFF_WORK, &n, img, len) != TINF_OK || n != pb->stage_raw) { dbg("[loader] inflate failed\n"); return -1; }
        img = (const u8 *)WUP_HANDOFF_WORK; len = n;
    }
    const hb_header2 *h = (const hb_header2 *)img;
    u32 n1 = len, n2 = 0, entry;
    if (len < sizeof *h || h->magic != HB_MAGIC) { dbg("[loader] not an app image\n"); return -1; }
    if (h->seg2_magic == HB_SEG2_MAGIC) {
        n1 = h->size; n2 = h->seg2_size;
        if (n1 > len || n1 + n2 != len || h->seg2_addr < HB_SEG2_START || h->seg2_addr + n2 > HB_SEG2_END) { dbg("[loader] bad second segment\n"); return -1; }
    }
    entry = h->entry;
    if (n1 > APP_HI - APP_LO || entry < APP_LO || entry >= APP_LO + n1) { dbg("[loader] bad app header\n"); return -1; }
    u32 a2 = h->seg2_addr;
    dbg("[loader] running "); dbg((const char *)h->name); dbg("\n");
    memcpy((void *)APP_LO, img, n1); sync_code(APP_LO, n1);
    if (n2) { memcpy((void *)a2, img + n1, n2); sync_code(a2, n2); }
    return ((int (*)(void))entry)();
}

/* ---- a whole system: X at power-on. The slot (0x90000: header {"GPST", size, crc32}, the image from 0x91000 to
   0xB0000, linked at RAM 0x10000; the free gap, clear of the menu's parts) holds a system that runs the pad on its own:
   the wup-homebrew kernel (wup-homebrew/kernel), which then runs its own build of the menu. Nintendo's firmware is stopped for good (until the next power-on): interrupts off, the SD host's DMA
   stopped, the caches cleaned and off, the MMU off; then the image goes to 0x10000 and gets the CPU with r0 = "HOOK".
   It sets the pad up again itself (its board init, the radio from reset). Without X, or with no valid image, nothing
   of this happens: the stock boot, or the menu, as always. */
#define SYS_HDR    0x00090000u
#define SYS_IMG    0x00091000u
#define SYS_END    0x000b0000u
#define SYS_MAGIC  0x54535047u            /* "GPST" */
#define SYS_RAM    0x00010000u
#define HOOK_ARG   0x4b4f4f48u            /* "HOOK" */
static u32 sys_size;
static int sys_stage(void) {             /* the system to the stage buffer, checked: 0 = there is none */
    u32 h[4];
    if (flash_read(SYS_HDR, h, sizeof h) || h[0] != SYS_MAGIC || h[1] < 0x100 || h[1] > SYS_END - SYS_IMG) return 0;
    if (flash_read(SYS_IMG, (void *)WUP_HANDOFF_STAGE, h[1])) return 0;
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, h[1]) != h[2]) { dbg("[loader] system CRC mismatch\n"); return 0; }
    sys_size = h[1];
    return 1;
}
__attribute__((target("arm"), noinline, noreturn)) static void sys_takeover(void) {
    dbg("[loader] system takeover\n");
    __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0xc0\n msr cpsr_c, r0" ::: "r0", "memory");     /* IRQ, FIQ off */
    *(volatile u8 *)(0xe001002fu) = 1;                 /* the SD host's software reset: its DMA stops (the radio's) */
    for (int i = 0; i < 100000 && (*(volatile u8 *)(0xe001002fu) & 1); i++) ;
    __asm__ volatile(
        "1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n bne 1b\n"     /* clean and invalidate the D-cache */
        "mcr p15, 0, %0, c7, c10, 4\n"                        /* drain the write buffer */
        "mrc p15, 0, r0, c1, c0, 0\n"
        "bic r0, r0, #0x1000\n bic r0, r0, #0x5\n"            /* I-cache, D-cache, MMU off */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mcr p15, 0, %0, c7, c7, 0\n"                         /* both caches invalidated */
        "mcr p15, 0, %0, c8, c7, 0\n"                         /* TLBs */
        :: "r"(0) : "r0", "memory", "cc");
    const u32 *s = (const u32 *)WUP_HANDOFF_STAGE;      /* (RAM is mapped flat: the same addresses with the MMU off) */
    u32 *d = (u32 *)SYS_RAM;
    for (u32 i = 0; i < (sys_size + 3) / 4; i++) d[i] = s[i];
    register u32 arg __asm__("r0") = HOOK_ARG;
    register u32 to __asm__("r1") = SYS_RAM;
    __asm__ volatile("bx r1" :: "r"(arg), "r"(to) : "memory");
    for (;;) ;
}
/* The radio, before the takeover. By now Nintendo's driver is loading its firmware into the radio, and a radio taken
   over in that state answers nothing on its backplane, whatever clock is asked for (seen on the pad, 2026-11-25). So
   it is reset first, by its own watchdog (chipcommon +0x80 = 1), written through Nintendo's driver's host-side
   backplane write (the "membytes" iovar: what the menu uses to patch the radio, menu/wifi/wifi_hw.c chip_write). The
   kernel then finds a chip just reset by its watchdog: the start its radio driver knows (wup-os, proven on the pad). */
#define DHD     ((int (*)(void *, void *, void *, u32))0x00041517)
#define DHD_PUB ((void *(*)(void))0x00045965)
static u32 ioc[7];
static u8 dbuf[64] __attribute__((aligned(32)));
static int chip_write32(u32 addr, u32 val) {
    /* "membytes\0" (9) + addr + size (17 bytes), the data after: placed so that the data lands on a word boundary
       (the driver copies it 32 bits at a time) */
    u8 req[17 + 4] __attribute__((aligned(4)));
    memcpy(req, "membytes", 9); memcpy(req + 9, &addr, 4); u32 n = 4; memcpy(req + 13, &n, 4); memcpy(req + 17, &val, 4);
    u8 *buf = dbuf + 3;
    u32 ln = 17 + 4 + 8;
    memset(buf, 0, ln); memcpy(buf, req, 17 + 4);
    ioc[0] = 3; ioc[1] = (u32)buf; ioc[2] = ln; ioc[3] = 1; ioc[4] = ioc[5] = ioc[6] = 0;
    cache_flush(dbuf, sizeof dbuf); cache_flush(ioc, sizeof ioc);
    int r = DHD(DHD_PUB(), ioc, buf, ln);
    cache_flush(dbuf, sizeof dbuf);
    return r;
}
static void radio_reset(void) {                        /* (the driver refuses while it is busy loading: tried for 3 s) */
    int r = -1, t = 0;
    for (; t < 3000 && (r = chip_write32(0x18000080u, 1)); t += 100) fw_sleep(100);
    dbg(r ? "[loader] the radio's watchdog: Nintendo's driver refused (error " : "[loader] the radio reset by its watchdog (after ");
    dbg_hex(r ? (u32)r : (u32)t); dbg(r ? ")\n" : " ms)\n");
    fw_sleep(50);
}
static void sys_work(void) {
    dbg_init();
    dbg("[loader] X held: the system in the slot\n");
    if (sys_stage()) { radio_reset(); sys_takeover(); }
    dbg("[loader] no system in the slot: the stock boot\n");
}

/* The firmware's task stack is small (inflating alone needs about 2 KB): the work runs on a stack of our own, the 3 KB
   above the hand-off block (0x34F400..0x350000). Apps switch to their own again. */
static void work(void);
__attribute__((target("arm"), noinline)) static void on_own_stack(void (*f)(void)) {
    __asm__ volatile("mov r4, sp\n\tmov sp, %1\n\tblx %0\n\tmov sp, r4"
                     :: "r"(f), "r"(0x00350000u) : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
}
static void loader_main(u8 *app_id) {
    u16 b = buttons();
    if (b & BTN_X) { on_own_stack(sys_work); return; }  /* (it returns only when there is no system: stock) */
    int a_held = (b & BTN_A) != 0, autoboot = cfg_autoboot();
    int want = a_held || autoboot;
    if ((u32)app_id == 1) want = 1;                     /* test call from the UART monitor (the hook passes &app_id) */
    if (want) on_own_stack(work);
}
static void work(void) {
    dbg_init();                                         /* UART1 for the log (not on a stock boot: nothing is touched then) */
    dbg("[loader] homebrew boot\n");
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    memset((void *)pb, 0, sizeof *pb);
    pb->magic = WUP_HANDOFF_MAGIC; pb->version = 2;
    for (int staged = 0;;) {
        if (!staged && !stage_menu(pb)) return;         /* (no menu: the stock boot) */
        int menu = !staged;
        int r = run_staged(pb);
        if (r < 0 && menu) return;
        if (!menu) pb->runs++;
        staged = r == WUP_HANDOFF_RUN;                      /* it staged the next one; else back to the menu */
        if (menu && !staged) { dbg("[loader] the menu returned: stock boot\n"); return; }
    }
}

__attribute__((section(".header"), used))
static const struct { u32 magic, entry; } header_ = { 0x31574643u /* "CFW1" */, (u32)loader_main };
