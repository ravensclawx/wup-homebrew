/* clock: the date and time from the network (lwIP's SNTP client, pool.ntp.org, once Wi-Fi is joined and then every
 * hour), the time zone and the 12/24-hour choice (kept in /config/clock), and the time as the status bar shows it.
 * The pad keeps no time while it is off: until the network has answered there is no time to show. Time zones are
 * POSIX TZ rules, so the C library handles summer time itself. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "menu.h"
#include "fs.h"

const clock_zone_t clock_zones[] = {
    { "UTC", "UTC", "UTC0" },
    { "Pacific/Honolulu", "Honolulu", "HST10" },
    { "America/Anchorage", "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "Los Angeles, Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Phoenix", "Phoenix", "MST7" },
    { "America/Denver", "Denver, Calgary", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "Chicago, Dallas", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "Mexico City", "CST6" },
    { "America/New_York", "New York, Toronto", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Bogota", "Bogotá, Lima", "<-05>5" },
    { "America/Halifax", "Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "America/St_Johns", "St. John's", "NST3:30NDT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo", "São Paulo, Buenos Aires", "<-03>3" },
    { "Europe/London", "London, Dublin, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Berlin", "Berlin, Paris, Rome, Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Lagos", "Lagos", "WAT-1" },
    { "Europe/Athens", "Athens, Helsinki, Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Africa/Johannesburg", "Johannesburg", "SAST-2" },
    { "Europe/Moscow", "Moscow, Istanbul, Nairobi", "MSK-3" },
    { "Asia/Dubai", "Dubai", "<+04>-4" },
    { "Asia/Kolkata", "Mumbai, Delhi", "IST-5:30" },
    { "Asia/Bangkok", "Bangkok, Jakarta", "<+07>-7" },
    { "Asia/Shanghai", "Beijing, Singapore, Perth", "CST-8" },
    { "Asia/Tokyo", "Tokyo, Seoul", "JST-9" },
    { "Australia/Adelaide", "Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "Brisbane", "AEST-10" },
    { "Australia/Sydney", "Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
const int clock_nzones = (int)(sizeof clock_zones / sizeof clock_zones[0]);

#define CLK1 0x314b4c43u                               /* "CLK1" */
static struct { uint32_t magic; char zone[40]; uint8_t h24, spare[3]; } cfg;
static int zone_i, sntp_on;

void clock_load(void) {
    if (fs_read("/config/clock", 0, &cfg, sizeof cfg) != (int)sizeof cfg || cfg.magic != CLK1) { memset(&cfg, 0, sizeof cfg); strcpy(cfg.zone, "UTC"); }
    zone_i = 0;
    for (int i = 0; i < clock_nzones; i++) if (!strcmp(cfg.zone, clock_zones[i].id)) zone_i = i;
    wup_time_zone(clock_zones[zone_i].tz);
#ifdef MENU_PREVIEW                                    /* (pictures from the emulator, which has no network) */
    if (!wup_time_known()) wup_time_set(1794994860, 0);        /* Wed Nov 18 2026, 9:41 */
#endif
}
static void save(void) { cfg.magic = CLK1; lfs_mkdir(&fs_lfs, "/config"); fs_write("/config/clock", &cfg, sizeof cfg); }
int clock_zone(void) { return zone_i; }
void clock_set_zone(int i) {
    if (i < 0 || i >= clock_nzones) return;
    zone_i = i; strncpy(cfg.zone, clock_zones[i].id, sizeof cfg.zone - 1); save();
    wup_time_zone(clock_zones[i].tz);
}
int clock_24h(void) { return cfg.h24; }
void clock_set_24h(int on) { cfg.h24 = (uint8_t)!!on; save(); }

/* SNTP: started the first time there is an address (libwup: wup_net_time; lwIP asks again every hour by itself) */
void clock_network(int online) {
    if (!online || sntp_on) return;
    wup_net_time();
    sntp_on = 1;
}

/* "9:41" (12-hour, as a status bar shows it: no AM/PM) or "21:41"; long: "Tue, Nov 18, 9:41 PM" or "Tue, Nov 18, 21:41";
   "" while the time is not known */
int clock_text(char *out, int n, int long_form) {
    static const char *const day[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!wup_time_known()) { if (n) out[0] = 0; return 0; }
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    int h = tm.tm_hour;
    if (!cfg.h24) { h %= 12; if (!h) h = 12; }
    char hm[12]; snprintf(hm, sizeof hm, cfg.h24 ? "%02d:%02d" : "%d:%02d", h, tm.tm_min);
    if (!long_form) return snprintf(out, (size_t)n, "%s", hm);
    return snprintf(out, (size_t)n, "%s, %s %d, %s%s", day[tm.tm_wday], mon[tm.tm_mon], tm.tm_mday, hm, cfg.h24 ? "" : tm.tm_hour < 12 ? " AM" : " PM");
}
