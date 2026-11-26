/* wifi_hw: the GamePad's Broadcom BCM4319 radio, driven through the stock firmware's own driver (LVC
 * rev 7677), from code running on the pad. C port of tools/wifi_probe.py + tools/wifi_join.py; see there
 * for how each step was found.
 *   wl_ioctl(cmd, buf, len, set)   LVC 0x1b165: radio ioctls. Keep buffers well under the driver's
 *                                  0x36c0-byte ioctl room (larger requests stop the pad with 165-3423).
 *   dhd_ioctl(dhdp, ioc, buf, len) LVC 0x41517, dhdp = 0x45965(): host-driver iovars ('membytes' =
 *                                  radio chip RAM = WIFI blob offset - 0x20).
 * Never WLC_DOWN: the interface then refuses to come back up until a power cycle. */
#include "wifi_hw.h"
#include "armcpu.h"
void wdbg(const char *s, int v);              /* progress over the UART (hb_wifi.c) */

typedef int (*wl_fn)(u32 cmd, void *buf, u32 len, u32 set);
typedef int (*dhd_fn)(void *dhdp, void *ioc, void *buf, u32 len);
typedef void *(*pub_fn)(void);
#define WL       ((wl_fn)0x0001b165)
#define DHD      ((dhd_fn)0x00041517)
#define DHD_PUB  ((pub_fn)0x00045965)
#define DLY_     ((void (*)(int))0x00089ae4)
void (*wifi_wait)(int ms);                     /* set by the menu: its screen keeps moving while the radio takes its time */
static void DLY(int ms) {
    if (wifi_wait) { wifi_wait(ms); return; }
    while (ms > 0) { DLY_(ms > 100 ? 100 : ms); ms -= 100; }   /* firmware callers use 1..200 */
}

enum { WLC_UP = 2, WLC_GET_INFRA = 19, WLC_SET_INFRA = 20, WLC_SET_AUTH = 22, WLC_GET_BSSID = 23,
       WLC_GET_SSID = 25, WLC_SET_SSID = 26, WLC_SCAN = 50, WLC_SCAN_RESULTS = 51, WLC_GET_RSSI = 127,
       WLC_SET_WSEC = 134, WLC_GET_UP = 162, WLC_SET_WPA_AUTH = 165, WLC_GET_VAR = 262, WLC_SET_VAR = 263,
       WLC_SET_PM = 86, WLC_SET_WSEC_PMK = 268 };

#define BUFSZ 0x2000                 /* the Python tools used this; larger risks the 165-3423 stop */
static u8 iobuf[BUFSZ + 64] __attribute__((aligned(32)));
static u32 ioc[7];

/* The buffer may live in cached RAM (the Homebrew Menu caches its pages): write it back and drop it from
   the D-cache around every driver call, in case the driver hands it to the SDIO DMA directly. */
static void bzero_(void *d, u32 n) { u8 *p = d; while (n--) *p++ = 0; }
static void bcopy_(void *d, const void *s, u32 n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; }
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static int ioctl_(u32 cmd, const void *data, u32 dlen, u32 n, int set) {
    if (n > BUFSZ) return -100;
    bzero_(iobuf, n); if (data) bcopy_(iobuf, data, dlen < n ? dlen : n);
    cache_flush(iobuf, n);
    int r = WL(cmd, iobuf, n, set);
    cache_flush(iobuf, n);
    return r;
}
static int setint(u32 cmd, int v) { return ioctl_(cmd, &v, 4, 4, 1); }
static int getint(u32 cmd, int *v) { int r = ioctl_(cmd, 0, 0, 4, 0); *v = *(int *)iobuf; return r; }
static int setvar(const char *name, const void *val, u32 vlen) {
    u8 req[64] __attribute__((aligned(4))); u32 n = slen(name) + 1;
    bcopy_(req, name, n); bcopy_(req + n, val, vlen);
    return ioctl_(WLC_SET_VAR, req, n + vlen, n + vlen, 1);
}
static int setvar_int(const char *name, int v) { return setvar(name, &v, 4); }

/* ---- radio chip RAM through the host driver's 'membytes' iovar */
static int dhd_(u32 cmd, u8 *buf, const void *req, u32 rlen, u32 n) {
    u32 ln = (rlen > n ? rlen : n) + 8;
    bzero_(buf, ln); bcopy_(buf, req, rlen);
    ioc[0] = cmd; ioc[1] = (u32)buf; ioc[2] = ln; ioc[3] = cmd == 3; ioc[4] = ioc[5] = ioc[6] = 0;
    cache_flush(buf, ln); cache_flush(ioc, sizeof ioc);
    int r = DHD(DHD_PUB(), ioc, buf, ln);
    cache_flush(buf, ln);
    return r;
}
int wifi_chip_read(u32 addr, void *out, u32 n) {
    u8 req[17] __attribute__((aligned(4))); bcopy_(req, "membytes", 9); bcopy_(req + 9, &addr, 4); bcopy_(req + 13, &n, 4);
    int r = dhd_(2, iobuf, req, 17, n);
    bcopy_(out, iobuf, n);
    return r;
}
static int chip_write(u32 addr, const void *data, u32 n) {
    /* the driver hands the data pointer to 32-bit copies; on ARMv5 an unaligned word load rotates bytes,
       so place "membytes\0" (9) + addr + size (17 bytes) such that the data lands on a word boundary */
    u8 req[17 + 16] __attribute__((aligned(4))); bcopy_(req, "membytes", 9); bcopy_(req + 9, &addr, 4); bcopy_(req + 13, &n, 4);
    bcopy_(req + 17, data, n);
    return dhd_(3, iobuf + 3, req, 17 + n, 4);
}

/* Radio firmware patches in chip RAM (nothing written to flash; a power cycle undoes them):
 * standard WPA2 RSN OUI instead of Nintendo's, and the PTK written unrotated. */
static const struct { u32 addr; u8 len, old[4], new_[4]; } patches[] = {
    { 0x297f2, 3, {0xa4, 0xc0, 0xe1}, {0x00, 0x0f, 0xac} },
    { 0x2afef, 3, {0xa4, 0xc0, 0xe1}, {0x00, 0x0f, 0xac} },
    { 0x2aff5, 3, {0xa4, 0xc0, 0xe1}, {0x00, 0x0f, 0xac} },
    { 0x2affb, 3, {0xa4, 0xc0, 0xe1}, {0x00, 0x0f, 0xac} },
    { 0x24c1c, 2, {0xe1, 0x1c}, {0x21, 0x1c} },                   /* adds r1, r4, #3 -> #0 */
    { 0x24c1e, 4, {0xaa, 0xf1, 0x03, 0x02}, {0xaa, 0xf1, 0x00, 0x02} },   /* sub.w r2, r10, #3 -> #0 */
    { 0x24c30, 4, {0xdc, 0xf7, 0xbe, 0xfd}, {0x00, 0xbf, 0x00, 0xbf} },   /* bl memcpy -> nop nop */
};
int wifi_patch_radio(void) {
    int changed = 0;
    for (unsigned i = 0; i < sizeof patches / sizeof patches[0]; i++) {
        u32 a = patches[i].addr, first = a & ~3u, last = (a + patches[i].len - 1) & ~3u, n = last - first + 4;
        u8 w[12], back[12];
        if (wifi_chip_read(first, w, n)) return -1;
        u8 *cur = w + (a - first);
        int is_new = 1, ok = 1;
        for (int k = 0; k < patches[i].len; k++) {
            if (cur[k] != patches[i].new_[k]) is_new = 0;
            if (cur[k] != patches[i].old[k] && cur[k] != patches[i].new_[k]) ok = 0;
        }
        if (is_new) continue;
        if (!ok) return -2 - (int)i;                /* unexpected bytes: not this radio firmware */
        bcopy_(cur, patches[i].new_, patches[i].len);
        for (u32 o = 0; o < n; o += 4) chip_write(first + o, w + o, 4);
        wifi_chip_read(first, back, n);
        for (u32 k = 0; k < n; k++) if (back[k] != w[k]) return -20 - (int)i;
        changed++;
    }
    return changed;
}

/* Bring the interface up once. Country/nmode must be set before the first WLC_UP. Legacy (no 802.11n):
 * this 20 MHz-only radio drops 40/80 MHz networks as join candidates unless HT is off, and then its ROM
 * faults on HT rates, so pin the 5 GHz unicast/multicast rates (54 / 6 Mbit/s). */
int wifi_bringup(const char *country) {
    int up = 0;
    getint(WLC_GET_UP, &up);
    if (!up) {
        u8 cc[12] __attribute__((aligned(4))); bzero_(cc, 12); bcopy_(cc, country, 2); *(int *)(cc + 4) = -1; bcopy_(cc + 8, country, 2);
        setvar("country", cc, 12);
        setvar_int("nmode", 0);
        setint(WLC_SET_INFRA, 1);
        setvar_int("mpc", 0);
        int r = setint(WLC_UP, 1);
        wdbg("WLC_UP", r);
        if (r) return r;
        DLY(500);
    }
    setvar_int("a_mrate", 12);
    setvar_int("a_rate", 108);
    setint(WLC_SET_PM, 0);       /* power saving off: on, the radio dozed between beacons and frames waited up to ~1 s
                                    (measured in wup-os on the same radio and firmware) */
    getint(WLC_GET_UP, &up);
    return up ? 0 : -1;
}

int wifi_scan(wifi_net *out, int max) {
    u8 p[68] __attribute__((aligned(4))); bzero_(p, sizeof p);
    for (int i = 0; i < 6; i++) p[36 + i] = 0xff;                 /* bssid broadcast */
    p[42] = 2;                                                    /* any BSS type */
    p[43] = 0;                                                    /* active */
    for (int i = 0; i < 4; i++) *(int *)(p + 44 + 4 * i) = -1;    /* nprobes, active/passive/home time */
    { u32 sp; __asm__ volatile("mov %0, sp" : "=r"(sp)); wdbg("sp", (int)sp); }
    DLY(1000); DLY(1000);                                         /* let the radio settle after WLC_UP */
    u32 q = 60; int rc = ioctl_(217 /* WLC_GET_VALID_CHANNELS */, &q, 4, 256, 0);
    wdbg("channels", rc); wdbg("n channels", *(int *)iobuf);
    wdbg("scan request", 0);
    int r = ioctl_(WLC_SCAN, p, sizeof p, sizeof p, 1);
    wdbg("scan start", r);
    if (r) return r < 0 ? r : -r;
    DLY(200); for (int i = 0; i < 34; i++) DLY(200);              /* the scan visits all channels (~7 s) */
    u32 hdr[3] = { BUFSZ, 0, 0 };
    wdbg("scan wait done", 0);
    r = ioctl_(WLC_SCAN_RESULTS, hdr, 12, BUFSZ, 0);
    wdbg("scan results", r);
    if (r) return -1;
    u32 buflen = *(u32 *)iobuf, count = *(u32 *)(iobuf + 8), off = 12;
    wdbg("bss count", (int)count);
    if (buflen > BUFSZ) buflen = BUFSZ;
    int n = 0;
    for (u32 i = 0; i < count && off + 80 <= buflen; i++) {
        u8 *b = iobuf + off; u32 blen = *(u32 *)(b + 4);
        u32 sl = b[18] > 32 ? 32 : b[18];
        int ch = *(u16 *)(b + 72) & 0xff, rssi = *(int16_t *)(b + 78), sec = (*(u16 *)(b + 16) & 0x10) != 0;
        if (sl && ch >= 36) {                                     /* 5 GHz, not hidden */
            int j;
            for (j = 0; j < n; j++) if (out[j].ssid_len == sl && !__builtin_memcmp(out[j].ssid, b + 19, sl)) break;
            if (j == n && n < max) { n++; out[j].rssi = -200; }
            if (j < n && rssi > out[j].rssi) {                    /* keep the strongest BSS per SSID */
                bcopy_(out[j].ssid, b + 19, sl); out[j].ssid[sl] = 0; out[j].ssid_len = sl;
                bcopy_(out[j].bssid, b + 8, 6); out[j].channel = ch; out[j].rssi = rssi; out[j].secured = sec;
            }
        }
        if (!blen) break;
        off += blen;
    }
    for (int i = 1; i < n; i++)                                   /* strongest first */
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) { wifi_net t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

int wifi_join(const wifi_net *net, const u8 *pmk) {
    int r = setint(WLC_SET_AUTH, 0);
    r |= setint(WLC_SET_WSEC, pmk ? 4 : 0);                       /* AES */
    r |= setint(WLC_SET_WPA_AUTH, pmk ? 0x80 : 0);                /* WPA2-PSK */
    r |= setvar_int("sup_wpa", pmk ? 1 : 0);
    if (pmk) {
        u8 k[68] __attribute__((aligned(4))); bzero_(k, sizeof k); k[0] = 32; bcopy_(k + 4, pmk, 32);
        r |= ioctl_(WLC_SET_WSEC_PMK, k, sizeof k, sizeof k, 1);
        bzero_(k, sizeof k);
    }
    u8 s[36] __attribute__((aligned(4))); bzero_(s, sizeof s); *(u32 *)s = net->ssid_len; bcopy_(s + 4, net->ssid, net->ssid_len);
    r |= ioctl_(WLC_SET_SSID, s, sizeof s, sizeof s, 1);
    return r;
}

/* 1 when associated to the network we asked for (WLC_GET_BSSID fails until then). Keying happens in
 * the radio; a wrong password still associates briefly, so success is judged by DHCP afterwards. */
int wifi_associated(const wifi_net *net) {
    if (ioctl_(WLC_GET_BSSID, 0, 0, 8, 0)) return 0;
    int any = 0; for (int i = 0; i < 6; i++) any |= iobuf[i];
    if (!any) return 0;
    if (ioctl_(WLC_GET_SSID, 0, 0, 36, 0)) return 0;
    u32 n = *(u32 *)iobuf;
    return n == net->ssid_len && !__builtin_memcmp(iobuf + 4, net->ssid, n);
}

int wifi_rssi(void) { int v = 0; getint(WLC_GET_RSSI, &v); return v; }

int wifi_mac(u8 *mac) {
    u8 req[48] __attribute__((aligned(4))); bzero_(req, sizeof req); bcopy_(req, "cur_etheraddr", 14);
    int r = ioctl_(WLC_GET_VAR, req, sizeof req, sizeof req, 0);
    bcopy_(mac, iobuf, 6);
    return r;
}
int wifi_leave(void) { return ioctl_(52 /* WLC_DISASSOC */, 0, 0, 0, 1); }
