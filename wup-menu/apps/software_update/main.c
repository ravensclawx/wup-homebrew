/* Software Update: keeps the system current, the catalogue's "system" packages (common/catalog.c): App Store, Software
 * Update and the Homebrew Menu. The apps install as the App Store installs; the menu's zip holds homebrew_menu.bin (the
 * menu slot as stored), written into the slot (libwup's wup_system.h: the header last, once the CRC agrees), last. When
 * this app ends the loader starts the menu from the slot: the new one at once. The running menu's version comes in the
 * hand-off block (menu_version). A system app missing from the pad is installed again here.
 * All centred, as the Wii U's System Update:
 *   Checking for Updates... -> Up to Date, or Update Available (A Install)
 *   Installing... (with a bar) -> Update Complete (B Done), or why not */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wup_kit.h"
#include "catalog.h"
#include "handoff.h"
#include "wup_system.h"
#include "fs.h"

WUP_APP("Software Update");

static enum { CHECKING, NONE, AVAILABLE, INSTALLING, UPDATED, FAILED } state;
static int err, installing_failed, nup, item;
static const catalog_pkg *up[4];
static char running[16];
static lv_obj_t *bar;

static int is_menu(const catalog_pkg *p) { return !strcmp(p->name, "homebrew_menu"); }
static void check(void) {
    err = catalog_load(); nup = 0;
    for (int pass = 0; pass < 2; pass++)                /* (the apps, then the menu: it goes last) */
        for (int i = 0; !err && i < catalog_count; i++) {
            const catalog_pkg *p = &catalog[i];
            if (!catalog_system(p) || is_menu(p) != pass || nup == 4) continue;
            if (catalog_newer(p->version, pass ? running : p->installed)) up[nup++] = p;
        }
    state = err ? FAILED : nup ? AVAILABLE : NONE;
    installing_failed = 0;
    kit_show();
}

/* the zip's homebrew_menu.bin into the slot: its first 4 KB the header sector, then the stored menu. On the
   wup-homebrew kernel the zip holds kernel.bin too (the kernel's slot as stored, the same shape), ahead of the menu:
   a new kernel with the old menu still runs it (the kernel's table only grows), and a cut-off update leaves no whole
   kernel or no whole menu: then A at power-on starts stock, and Software Update can run again from there */
typedef struct { int mine, kernel, first, written; uint32_t got; } slot_t;
static int s_begin(void *arg, const char *path, uint32_t size) {
    slot_t *x = arg; x->kernel = 0; x->mine = !strcmp(path, "homebrew_menu.bin");
    if (!strcmp(path, "kernel.bin")) x->mine = x->kernel = 1;
    x->first = 1; x->got = 0;
    return x->mine && size < 4096 + 64 ? CATALOG_EDAMAGED : 0;
}
static int s_data(void *arg, const uint8_t *d, uint32_t n) {
    slot_t *x = arg;
    if (!x->mine) return 0;
    if (x->first) {                                  /* (pieces are 4 KB: the first is the header sector) */
        x->first = 0;
        if (x->kernel) return n != 4096 || wup_kernel_slot_begin(d) ? CATALOG_EDAMAGED : 0;
        if (n != 4096 || wup_menu_slot_begin(d)) return CATALOG_EDAMAGED;
        return 0;
    }
    if (bar) lv_bar_set_value(bar, (item * 100 + 90 + (int)(x->got / 30000 > 10 ? 10 : x->got / 30000)) / nup, LV_ANIM_OFF);
    x->got += n;
    return wup_menu_slot_write(d, n) ? CATALOG_EWRITE : 0;
}
static int s_end(void *arg) {
    slot_t *x = arg;
    if (!x->mine) return 0;
    if (wup_menu_slot_end()) return CATALOG_EWRITE;  /* (the CRC agrees: the new menu is the slot's) */
    if (!x->kernel) x->written = 1;                  /* (the menu: last, the update is whole) */
    return 0;
}
static void progress(int pct) {                  /* (each update its share of the bar; the menu's last tenth: writing) */
    if (bar) lv_bar_set_value(bar, (item * 100 + (is_menu(up[item]) ? pct * 9 / 10 : pct)) / nup, LV_ANIM_OFF);
}
static int install_menu(const catalog_pkg *p) {
    int r = catalog_fetch(p, progress);
    if (!r) {
        static const catalog_sink sink = { s_begin, s_data, s_end };
        slot_t x = { 0 };
        r = catalog_unzip(&sink, &x);
        if (!r && !x.written) r = CATALOG_EDAMAGED;
        fs_remove(ZIP_FILE);
    }
    return r;
}
static void install(void) {
    err = 0;
    for (item = 0; !err && item < nup; item++) {
        err = is_menu(up[item]) ? install_menu(up[item]) : catalog_install(up[item], progress);
    }
    bar = 0; state = err ? FAILED : UPDATED; installing_failed = err != 0; kit_hold(state == UPDATED);
    kit_show();
}
static void start(void) { state = INSTALLING; kit_hold(1); kit_show(); kit_defer(install); }

/* all centred, as the Wii U's: a title, a quieter line at most */
static void screen(lv_obj_t *scr) {
    char t[48];
    switch (state) {
    case CHECKING: kit_busy(kit_page(scr, "Software Update", ""), "Checking for Updates\xE2\x80\xA6", "", NULL); kit_defer(check); break;
    case NONE: snprintf(t, sizeof t, "Homebrew Menu %s", running); kit_outcome(kit_page(scr, "Software Update", "B Back"), "Up to Date", t); break;
    case AVAILABLE: kit_outcome(kit_page(scr, "Software Update", "B Back    A Install"), "Update Available", ""); kit_on_a(start); break;
    case INSTALLING: {
        lv_obj_t *l = kit_page(scr, "Software Update", "");
        kit_busy(l, "Installing\xE2\x80\xA6", "Don't turn off the GamePad", NULL);
        bar = kit_bar(l);
        break;
    }
    case UPDATED:                                  /* (B ends the app: the loader starts the new menu) */
        kit_hold(0);
        kit_outcome(kit_page(scr, "Software Update", "B Done"), "Update Complete", "");
        break;
    case FAILED:
        kit_outcome(kit_page(scr, "Software Update", "B Back"), installing_failed ? "Couldn't Install" : "Can't Check for Updates", catalog_why(err));
        break;
    }
}

int main(void) {
    const wup_handoff_t *pb = wup_handoff();
    snprintf(running, sizeof running, "%s", pb && pb->menu_version[0] ? pb->menu_version : "0");
    catalog_idle = kit_idle;
    kit_start(screen);
    while (kit_step()) {}
    return 0;
}
