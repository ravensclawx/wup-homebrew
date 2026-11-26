/* The hand-off block between the kernel's app runner (kernel/src/app.c) and the apps it runs (WUP_HANDOFF_ADDR,
 * 0x7F000: just below the app's block, wup_ram.h).
 *
 * The menu is an app like any other. Nothing stays in memory between apps but the kernel: each app owns the whole
 * app block. When an app returns WUP_HANDOFF_RUN it has staged another image (libwup's wup_exec), which runs next;
 * after any other app the menu runs again from its flash slot.
 * A staged image waits at WUP_HANDOFF_STAGE (screen 1's memory), stage_size bytes, deflated when stage_raw is not 0
 * (its size inflated). The kernel inflates it straight into the app block (WUP_APP_BASE) and calls its entry. */
#pragma once
#include <stdint.h>
#include "wup_ram.h"
#define WUP_HANDOFF_MAGIC  0x42444150u          /* "PADB" */
#define WUP_HANDOFF_RUN    0x100                /* an app's return value: run the image I staged */
#define WUP_HANDOFF_STAGE  WUP_STAGE
#define WUP_HANDOFF_STAGE_MAX WUP_STAGE_MAX
#define WUP_HANDOFF_WORK_MAX (WUP_APP_END - WUP_APP_BASE)   /* the most an app image may inflate to */
enum {
    WUP_HO_RADIO_READY = 1,                     /* the radio is patched and up: the menu skips that step */
    WUP_HO_WIFI_JOINED = 2,                     /* it was associated when the app started */
    WUP_HO_NET_INFO = 8,                        /* net_* below hold the menu's live connection (libwup's networking) */
};
enum { WUP_UI_SET = 1, WUP_UI_DARK = 2, WUP_UI_QUIET = 4, WUP_UI_24H = 8 };   /* ui: set by the menu; dark appearance; no sound effects; 24-hour clock */
typedef struct {
    uint32_t magic, version;                /* WUP_HANDOFF_MAGIC, 2 */
    uint32_t flags, code_size;              /* WUP_HO_*; size of the app image at WUP_APP_BASE */
    uint32_t menu_sel, runs;                /* home-screen selection to restore; apps started this boot */
    char app_path[64];                      /* "/apps/x.wup" ("" from wuplink) */
    char arg[192];                          /* an argument for the app (e.g. a ROM path); "" = none */
    /* the network the menu was on (WUP_HO_NET_INFO): the app's lwIP takes it over without DHCP */
    uint32_t net_ip, net_mask, net_gw, net_dns;   /* as lwIP stores them (network byte order in memory) */
    uint8_t net_mac[6];
    uint8_t light;                          /* the menu's screen brightness + 1 (1..101 = 0..100 %; 0: not set) */
    uint8_t ui;                             /* the menu's look and sound for apps that match it: WUP_UI_* */
    uint32_t stage_size, stage_raw;         /* the staged image (WUP_HANDOFF_RUN): bytes at WUP_HANDOFF_STAGE; inflated size or 0 */
    /* (after the loader's part, which it clears at power-on: valid only with time_magic and time_check, src/time.c)
       the date and time at a reading of the 997.3 kHz counter (0xF0000408), and the time zone (a POSIX TZ rule) */
    uint32_t time_magic, time_check, time_s_lo, time_s_hi, time_us, time_tick;
    char time_zone[48];
    char menu_version[16];                  /* the Homebrew Menu that started this app: "1.0.0" (Software Update) */
} wup_handoff_t;
#define WUP_HANDOFF ((volatile wup_handoff_t *)WUP_HANDOFF_ADDR)
