/* Wi-Fi: SDHCI host controller (0xE0010000) + Broadcom BCM4319 SDIO dongle model.
 *
 * The real chip runs its own firmware (the WIFI flash partition), which the host downloads
 * into chip RAM. We don't emulate the chip's ARM core; instead, once the host releases the
 * core from reset, a fake "dongle" answers the SDPCM/CDC control protocol well enough for the
 * host driver to bring the interface up. It sees no networks. */
#include "dev.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void irq_raise(int n);
void irq_lower(int n);

static int wdbg = -1;
#define WLOG(...) do { if (wdbg < 0) wdbg = getenv("WIFI_DEBUG") != NULL; if (wdbg) fprintf(stderr, __VA_ARGS__); } while (0)

int wifi_irq = 2;    /* SD host interrupt line */

/* ======================================================================= card (SDIO) */
#define RAM_BYTES 0x80000
static uint8_t cccr[0x400];          /* CCCR + FBRs */
static uint8_t cis[0x4000];          /* CIS space 0x1000..0x4FFF */
static uint8_t f1regs[0x40];         /* 0x10000.. */
static uint32_t sbwindow;
static uint8_t *chipram;             /* dongle RAM at backplane 0 */
static bool arm_running, f2_ready;
static uint16_t rca = 0x0001;

/* backplane cores (Sonics Silicon Backplane, 4KB each from 0x18000000) */
enum { C_CC, C_SDIO, C_ARM, C_RAM, C_D11, NCORES };
static const uint16_t core_id[NCORES] = { 0x800, 0x829, 0x82a, 0x80e, 0x812 };
static const uint8_t  core_rev[NCORES] = { 17, 4, 2, 4, 15 };
static uint32_t coreregs[NCORES][0x400];

/* SDIO core (sdpcmd) register offsets */
#define SD_INTSTATUS 0x20
#define SD_HOSTINTMASK 0x24
#define SD_INTMASK 0x28
#define SD_TOSBMB 0x40
#define SD_TOHOSTMB 0x44
#define SD_TOSBMBDATA 0x48
#define SD_TOHOSTMBDATA 0x4c
#define I_HMB_FC_STATE (1u << 4)
#define I_HMB_FC_CHANGE (1u << 5)
#define I_HMB_FRAME_IND (1u << 6)
#define I_HMB_HOST_INT (1u << 7)

static void card_irq_update(void);

static uint32_t bp_read32_(uint32_t a);
static uint32_t bp_read32(uint32_t a) {
    uint32_t v = bp_read32_(a);
    if (a >= RAM_BYTES) WLOG("[wifi] core read  %08x -> %08x\n", a, v);
    return v;
}
static uint32_t bp_read32_(uint32_t a) {
    if (a < RAM_BYTES) return *(uint32_t *)(chipram + (a & ~3u));
    if (a >= 0x18000000 && a < 0x18000000 + NCORES * 0x1000) {
        int c = (a - 0x18000000) >> 12;
        uint32_t off = a & 0xffc;
        switch (off) {
        case 0xffc: return (0x4243u << 16) | (core_id[c] << 4) | (core_rev[c] & 0xf) | ((core_rev[c] & 0x30) << 8);
        case 0xff8: return 0x70000000u | (c == C_CC ? 0 : 0);   /* sbidlow */
        case 0xf80: return (0x18000000 + c * 0x1000) | (11 << 3); /* sbadmatch0: 4KB */
        case 0xf9c: return 0;                                      /* sbtmstatehigh */
        case 0xf90: return 0;                                      /* sbimstate */
        }
        if (c == C_CC) {
            switch (off) {
            case 0x00: return 0x4319 | (1u << 16) | (0u << 20) | ((uint32_t)NCORES << 24);
            }
        }
        if (c == C_RAM && off == 0x00) {
            /* coreinfo: 4 banks of 128KB: nb << 4 | bsz (bank size = 1 << (bsz + 14)) */
            return (4 << 4) | 3;
        }
        return coreregs[c][off / 4];
    }
    WLOG("[wifi] bp read %08x\n", a);
    return 0;
}

static uint32_t ram_written;
static void bp_write32(uint32_t a, uint32_t v) {
    if (a < RAM_BYTES) { *(uint32_t *)(chipram + (a & ~3u)) = v; ram_written += 4; return; }
    WLOG("[wifi] core write %08x <- %08x\n", a, v);
    if (a >= 0x18000000 && a < 0x18000000 + NCORES * 0x1000) {
        int c = (a - 0x18000000) >> 12;
        uint32_t off = a & 0xffc;
        if (c == C_SDIO && off == SD_INTSTATUS) { coreregs[c][off / 4] &= ~v; card_irq_update(); return; }
        coreregs[c][off / 4] = v;
        if (c == C_SDIO && (off == SD_HOSTINTMASK || off == SD_INTMASK)) card_irq_update();
        if (c == C_SDIO && off == SD_TOSBMB) WLOG("[wifi] tosbmailbox %08x\n", v);
        if (c == C_ARM && off == 0xf98) {
            bool run = (v & 0x30001) == 0x10000;   /* clock on, not in reset */
            if (run && !arm_running) {
                WLOG("[wifi] dongle ARM released from reset\n");
                arm_running = true;
            }
            if (!run) arm_running = false;
        }
        return;
    }
    WLOG("[wifi] bp write %08x <- %08x\n", a, v);
}

static uint8_t bp_read8(uint32_t a) { return bp_read32(a & ~3u) >> ((a & 3) * 8); }
static void bp_write8(uint32_t a, uint8_t v) {
    if (a < RAM_BYTES) { chipram[a] = v; ram_written++; return; }
    uint32_t w = bp_read32(a & ~3u);
    int sh = (a & 3) * 8;
    w = (w & ~(0xffu << sh)) | ((uint32_t)v << sh);
    bp_write32(a & ~3u, w);
}

/* ---------------------------------------------------------------- dongle (fake firmware) */
#define MAXFRAMES 32
typedef struct { uint8_t data[2048]; int len; } Frame;
static Frame rxq[MAXFRAMES]; static int rxq_head, rxq_n;
static int cur_pos = -1;                 /* read position within rxq[rxq_head] */
static uint8_t tx_seq_dongle;
static uint8_t host_maxseq = 0xff;

static void post_host_int(uint32_t bits) {
    coreregs[C_SDIO][SD_INTSTATUS / 4] |= bits;
    card_irq_update();
}

static void dongle_send(int chan, const uint8_t *payload, int plen) {
    if (rxq_n >= MAXFRAMES) return;
    Frame *f = &rxq[(rxq_head + rxq_n) % MAXFRAMES];
    /* data frames carry 2 pad bytes after the 12-byte SDPCM header (data offset 14), like the pad's own
       transmissions, so the IP header ends up 32-bit aligned: the firmware's IP stack does word loads */
    int hdr = chan == 2 ? 14 : 12, len = hdr + plen;
    memset(f->data, 0, sizeof f->data);
    f->data[0] = len; f->data[1] = len >> 8;
    f->data[2] = ~len; f->data[3] = (~len) >> 8;
    f->data[4] = tx_seq_dongle++;
    f->data[5] = chan & 0x0f;
    f->data[6] = 0;              /* next length */
    f->data[7] = hdr;            /* data offset */
    f->data[8] = 0;              /* flow control */
    f->data[9] = host_maxseq;    /* tx credit: max sequence host may send */
    memcpy(f->data + hdr, payload, plen);
    f->len = len;
    rxq_n++;
    post_host_int(I_HMB_FRAME_IND);
}

static const uint8_t mac[6] = { 0x00, 0x19, 0xfd, 0x12, 0x34, 0x56 };
uint8_t wifi_bssid[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x10 };  /* console MAC, from the SSID we're asked to join */
const uint8_t *wifi_pad_mac(void) { return mac; }

/* Broadcom event (SDPCM channel 1): BDC header + Ethernet (type 0x886c) + bcmeth_hdr + wl_event_msg,
 * all big endian. The firmware's handler (LVC 0x1d7cc) reads type/status/reason at +4/+8/+12. */
static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void dongle_event_full(uint32_t type, uint32_t status, uint32_t reason, uint16_t flags, const uint8_t *data, int dlen);
static void dongle_event(uint32_t type, uint32_t status, uint32_t reason, uint16_t flags) { dongle_event_full(type, status, reason, flags, NULL, 0); }
static void dongle_event_data(uint32_t type, const uint8_t *data, int dlen) { dongle_event_full(type, 0, 0, 0, data, dlen); }
static void dongle_event_full(uint32_t type, uint32_t status, uint32_t reason, uint16_t flags, const uint8_t *data, int dlen) {
    uint8_t f[4 + 14 + 10 + 48 + 64];
    if (dlen > 64) dlen = 64;
    memset(f, 0, sizeof f);
    f[0] = 0x20;                                   /* BDC version 2 */
    uint8_t *e = f + 4;
    memcpy(e, mac, 6); memcpy(e + 6, wifi_bssid, 6); be16(e + 12, 0x886c);
    uint8_t *b = e + 14;
    be16(b, 0x8001); be16(b + 2, 10 + 48 - 4 + dlen); b[4] = 0; b[5] = 0x00; b[6] = 0x10; b[7] = 0x18; be16(b + 8, 1);
    uint8_t *m = b + 10;
    be16(m, 1); be16(m + 2, flags); be32(m + 4, type); be32(m + 8, status); be32(m + 12, reason);
    be32(m + 16, 0); be32(m + 20, dlen); memcpy(m + 24, wifi_bssid, 6); strcpy((char *)m + 30, "wl0");
    if (dlen) memcpy(m + 48, data, dlen);
    if (type != 82) WLOG("[wifi] -> event %u status %u reason %u flags %u\n", type, status, reason, flags);
    dongle_send(1, f, 4 + 14 + 10 + 48 + dlen);
}

/* Ethernet frames to/from the firmware (SDPCM channel 2, BDC header) */
void net_from_pad(const uint8_t *eth, int len);
void wifi_net_inject(const uint8_t *eth, int len) {
    static uint8_t f[2048];
    if (len > (int)sizeof f - 4) return;
    f[0] = 0x20; f[1] = 0; f[2] = 0; f[3] = 0;
    memcpy(f + 4, eth, len);
    dongle_send(2, f, len + 4);
}
static int join_pending;   /* ticks until we report a successful association */
static bool linked;

/* The console's time base (802.11 TSF, microseconds). We use host wall-clock time so a console
   program on the same machine can stamp its video with the same clock (run with --realtime). */
#include <sys/time.h>
static uint64_t tsf_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec; }

/* Event 0x52 (82): periodic TSF report. The firmware's handler (LVC 0x1daf0) takes BE u32 data+8 as the
   console time and syncs the pad's 1 MHz clock to it (0x1cd4e: drift trim 27/9973, hard set if >8 ms off);
   data+0x10/+0x14 are stored as a 64-bit pair (used for deltas only; we put the 64-bit TSF there). */
static void dongle_event_data(uint32_t type, const uint8_t *data, int dlen);
static void send_tsf_event(void) {
    uint64_t t = tsf_now();
    uint8_t dd[0x18]; memset(dd, 0, sizeof dd);
    for (int i = 0; i < 4; i++) { dd[8 + i] = (uint8_t)(t >> (24 - 8 * i)); dd[0x14 + i] = (uint8_t)(t >> (24 - 8 * i)); dd[0x10 + i] = (uint8_t)(t >> (56 - 8 * i)); }
    dongle_event_data(82, dd, sizeof dd);
}

/* answer a CDC ioctl */
static void dongle_ioctl(const uint8_t *p, int len) {
    uint32_t cmd, dlen, flags;
    memcpy(&cmd, p, 4); memcpy(&dlen, p + 4, 4); memcpy(&flags, p + 8, 4);
    const uint8_t *data = p + 16;
    int avail = len - 16; if (avail < 0) avail = 0;
    bool set = flags & 2;
    static uint8_t out[1600];
    if (dlen > 1500) dlen = 1500;
    memset(out, 0, 16 + dlen);
    memcpy(out, p, 16);
    uint8_t *od = out + 16;
    if ((int)dlen <= avail) memcpy(od, data, dlen); else memcpy(od, data, avail);
    char name[64] = "";
    if (cmd == 262 || cmd == 263) { /* GET_VAR / SET_VAR: buffer starts with the name */
        int n = 0; while (n < 63 && n < avail && data[n]) { name[n] = data[n]; n++; } name[n] = 0;
    }
    WLOG("[wifi] ioctl %s cmd %u len %u %s\n", set ? "SET" : "GET", cmd, dlen, name);
    if (!set) {
        memset(od, 0, dlen);
        switch (cmd) {
        case 0: { uint32_t m = 0x14e46c77; memcpy(od, &m, 4); break; }      /* WLC_GET_MAGIC */
        case 1: { uint32_t v = 1; memcpy(od, &v, 4); break; }               /* WLC_GET_VERSION */
        case 262:
            if (!strcmp(name, "cur_etheraddr")) memcpy(od, mac, 6);
            else if (!strcmp(name, "ver")) strcpy((char *)od, "wl0: emulated 4.219.233.0");
            break;
        }
    }
    if (set && cmd == 26 && avail >= 36) {         /* WLC_SET_SSID: join. SSID "WiiU" + console MAC in hex */
        uint32_t sl; memcpy(&sl, data, 4);
        char ssid[33] = ""; if (sl > 32) sl = 32; memcpy(ssid, data + 4, sl); ssid[sl] = 0;
        WLOG("[wifi] join '%s'\n", ssid);
        if (sl == 16 && !strncmp(ssid, "WiiU", 4))
            for (int i = 0; i < 6; i++) { unsigned v; sscanf(ssid + 4 + i * 2, "%2x", &v); wifi_bssid[i] = v; }
        join_pending = 2000;
    }
    memset(out + 8, 0, 0); /* flags preserved (contains request id) */
    uint32_t status = 0; memcpy(out + 12, &status, 4);
    dongle_send(0, out, 16 + dlen);
}

/* host -> dongle frame on F2 */
static uint8_t txbuf[8192]; static int txlen;
static void dongle_rx_frame(const uint8_t *f, int n) {
    if (n < 12) return;
    int len = f[0] | (f[1] << 8);
    int chan = f[5] & 0x0f, doff = f[7];
    if (len > n) len = n;
    host_maxseq = f[4] + 8;
    if (chan == 0 && len - doff >= 16) dongle_ioctl(f + doff, len - doff);
    else if (chan == 2 && len - doff > 4) {              /* data: BDC header (4 + 4*dataOffset) + Ethernet */
        if (getenv("WIFI_TX_HDR")) {                     /* raw SDPCM + BDC header bytes of each data frame */
            static int hn; if (hn++ < 12) { fprintf(stderr, "[wifi] tx hdr len %d:", len); for (int i = 0; i < doff + 8 && i < len; i++) fprintf(stderr, " %02x", f[i]); fprintf(stderr, " | eth type %02x%02x\n", f[doff + 4 + 4 * f[doff + 3] + 12], f[doff + 4 + 4 * f[doff + 3] + 13]); }
        }
        const uint8_t *b = f + doff; int bl = 4 + b[3] * 4;
        { static int shown; if (shown++ < 3) WLOG("[wifi] pad data frame: doff %d bdc %02x %02x %02x %02x\n", doff, b[0], b[1], b[2], b[3]); }
        if (len - doff > bl) net_from_pad(b + bl, len - doff - bl);
        if (getenv("WIFI_TX_STACK")) {                    /* who sent this frame: code pointers on the stack */
            static int n; extern CPU cpu; extern uint32_t cpu_peek32(CPU *, uint32_t);
            if (n++ < 40) {
                const uint8_t *e = b + bl;
                fprintf(stderr, "[wifi] tx frame type %02x%02x len %d  pc %08x lr %08x stack:", e[12], e[13], len - doff - bl, cpu.cur_pc, cpu.r[14]);
                for (int i = 0; i < 96; i++) { uint32_t v = cpu_peek32(&cpu, cpu.r[13] + i * 4); if ((v & 1) && v > 0x8000 && v < 0xc0000) fprintf(stderr, " %08x", v); }
                fprintf(stderr, "\n");
            }
        }
    }
    else WLOG("[wifi] host frame chan %d len %d\n", chan, len);
}

void net_poll(void);
static void dongle_tick(void) {
    if (join_pending && !--join_pending) {
        /* Successful WPA2-PSK join. The firmware's handler (LVC 0x1d7cc) sets one flag bit per event:
           ASSOC bit0, LINK bit2, SET_SSID bit1 / JOIN bit3 (only after LINK), PSK_SUP keyed bit4 (only after
           LINK); with WPA it declares the link up once all of 0x1f are set (check at 0x1dbee). */
        dongle_event(7, 0, 0, 0);          /* WLC_E_ASSOC */
        dongle_event(16, 0, 0, 1);         /* WLC_E_LINK, flags: link up */
        dongle_event(0, 0, 0, 0);          /* WLC_E_SET_SSID, success */
        dongle_event(1, 0, 0, 0);          /* WLC_E_JOIN */
        dongle_event(46, 6, 0, 0);         /* WLC_E_PSK_SUP, WLC_SUP_KEYED */
        linked = true;
    }
    static int np; if (++np >= 256) { np = 0; net_poll(); }
    static int nt; if (linked && ++nt >= 11000) { nt = 0; send_tsf_event(); }   /* ~100 ms (ticks are ~1000 cycles) */
    if (arm_running && !f2_ready) {
        f2_ready = true;
        WLOG("[wifi] dongle firmware 'ready'\n");
        /* firmware-ready mailbox: SDPCM protocol version 4 */
        coreregs[C_SDIO][SD_TOHOSTMBDATA / 4] = (4u << 16) | 0x8;
        post_host_int(I_HMB_HOST_INT);
    }
}

/* ---------------------------------------------------------------- SDIO functions */
static uint8_t f0_read(uint32_t a) {
    if (a >= 0x1000 && a < 0x5000) return cis[a - 0x1000];
    if (a < sizeof cccr) {
        if (a == 0x03) return cccr[0x02] & (0x02 | (f2_ready ? 0x04 : 0));     /* IORDY */
        if (a == 0x05) {                                                       /* INTPEND */
            uint32_t s = coreregs[C_SDIO][SD_INTSTATUS / 4] & coreregs[C_SDIO][SD_HOSTINTMASK / 4];
            return s ? 0x02 : 0;
        }
        return cccr[a];
    }
    return 0;
}
static void f0_write(uint32_t a, uint8_t v) {
    if (a < sizeof cccr) {
        if (a == 0x06 && (v & 0x08)) { WLOG("[wifi] card reset\n"); }
        cccr[a] = v;
        card_irq_update();
    }
}

/* WIFI_ISO=1: the radio as stock's standby leaves it (on the pad, 2026-11-25): DEVICE_CTL 0x08, pads isolated, its
   backplane unreachable while that bit is set */
static void iso_init(void) {
    static int done; if (done) return; done = 1;
    if (getenv("WIFI_ISO")) f1regs[0x09] = 0x08;
}
static uint8_t f1_read(uint32_t a) {
    iso_init();
    if (a < 0x10000) return (f1regs[0x09] & 0x08) ? 0xff : bp_read8(sbwindow + (a & 0x7fff));
    if (a != 0x1000e) WLOG("[wifi] f1 reg %05x read -> %02x\n", a, a - 0x10000 < sizeof f1regs ? f1regs[a - 0x10000] : 0);
    if (a - 0x10000 < sizeof f1regs) {
        uint32_t r = a - 0x10000;
        if (r == 0x0e) {   /* CHIPCLKCSR */
            uint8_t v = f1regs[r];
            if (v & 0x09) v |= 0x40;          /* ALP available */
            if (v & 0x12) v |= 0xc0;          /* HT available */
            return v;
        }
        return f1regs[r];
    }
    return 0;
}
static void f1_write(uint32_t a, uint8_t v) {
    if (a < 0x10000) { bp_write8(sbwindow + (a & 0x7fff), v); return; }
    uint32_t r = a - 0x10000;
    if (r < sizeof f1regs) {
        f1regs[r] = v;
        WLOG("[wifi] f1 reg %05x <- %02x (ram written so far %u)\n", a, v, ram_written);
        if (r >= 0x0a && r <= 0x0c)
            sbwindow = ((f1regs[0x0a] & 0x80) << 8) | (f1regs[0x0b] << 16) | ((uint32_t)f1regs[0x0c] << 24);
    }
}

/* multi-byte F1 access (4-byte aligned register accesses through the window) */
static void f1_read_block(uint32_t a, uint8_t *buf, int n, bool inc) {
    iso_init();
    if (a < 0x10000 && (f1regs[0x09] & 0x08)) { memset(buf, 0xff, n); return; }
    if (a < 0x10000 && n == 4 && !(a & 3)) {
        uint32_t v = bp_read32(sbwindow + (a & 0x7fff));
        memcpy(buf, &v, 4); return;
    }
    for (int i = 0; i < n; i++) buf[i] = f1_read(inc ? a + i : a);
}
static void f1_write_block(uint32_t a, const uint8_t *buf, int n, bool inc) {
    iso_init();
    if (a < 0x10000 && (f1regs[0x09] & 0x08)) { memset(buf, 0xff, n); return; }
    if (a < 0x10000 && n == 4 && !(a & 3)) {
        uint32_t v; memcpy(&v, buf, 4);
        bp_write32(sbwindow + (a & 0x7fff), v); return;
    }
    for (int i = 0; i < n; i++) f1_write(inc ? a + i : a, buf[i]);
}

static void f2_read_block(uint8_t *buf, int n) {
    memset(buf, 0, n);
    if (cur_pos < 0) {
        if (!rxq_n) return;
        cur_pos = 0;
    }
    Frame *f = &rxq[rxq_head];
    int left = f->len - cur_pos;
    int k = left < n ? left : n;
    if (k > 0) memcpy(buf, f->data + cur_pos, k);
    cur_pos += n;
    if (cur_pos >= f->len) {
        cur_pos = -1;
        rxq_head = (rxq_head + 1) % MAXFRAMES; rxq_n--;
        if (!rxq_n) coreregs[C_SDIO][SD_INTSTATUS / 4] &= ~I_HMB_FRAME_IND;
    }
}
static void f2_write_block(const uint8_t *buf, int n) {
    if (txlen + n > (int)sizeof txbuf) txlen = 0;
    memcpy(txbuf + txlen, buf, n);
    txlen += n;
    int flen = txbuf[0] | (txbuf[1] << 8);
    if (txlen >= flen && flen >= 12) { dongle_rx_frame(txbuf, flen); txlen = 0; }
    else if (flen < 12) txlen = 0;
}

static bool card_irq_pending(void) {
    if (!(cccr[0x04] & 0x01)) return false;     /* master interrupt enable */
    uint32_t s = coreregs[C_SDIO][SD_INTSTATUS / 4] & coreregs[C_SDIO][SD_HOSTINTMASK / 4];
    return s && (cccr[0x04] & 0x02);
}

/* ======================================================================= SDHCI host */
static uint8_t hc[0x100];
#define HC16(o) (*(uint16_t *)&hc[o])
#define HC32(o) (*(uint32_t *)&hc[o])
static uint8_t xfer[8192]; static int xfer_len, xfer_pos, xfer_blksz, xfer_blocks_left;
static bool xfer_read, xfer_active;
static uint32_t xfer_arg;   /* CMD53 argument for writes */

static void hc_irq_update(void) {
    uint16_t nis = HC16(0x30), eis = HC16(0x32);
    /* card interrupt is level-sensitive */
    if (card_irq_pending() && (HC16(0x34) & 0x100)) nis |= 0x100; else nis &= ~0x100;
    if (eis) nis |= 0x8000; else nis &= ~0x8000;
    HC16(0x30) = nis;
    bool irq = (nis & HC16(0x38) & 0x7fff) || (eis & HC16(0x3a));
    static bool prev;
    if (irq && !prev) irq_raise(wifi_irq);          /* edge-triggered */
    prev = irq;
}
static void card_irq_update(void) { hc_irq_update(); }

static void set_int(uint16_t bits) { HC16(0x30) |= bits; hc_irq_update(); }
static void set_err(uint16_t bits) { HC16(0x32) |= bits; hc_irq_update(); }

static void cmd53_do_read(void);

static void sd_command(uint16_t cmdreg) {
    int idx = (cmdreg >> 8) & 0x3f;
    uint32_t arg = HC32(0x08);
    uint32_t resp = 0;
    bool ok = true, data = cmdreg & 0x20;
    memset(&hc[0x10], 0, 16);
    if (idx == 53 || (idx == 52 && ((arg >> 28) & 7) != 0))
        WLOG("[wifi] CMD%d arg %08x (%s fn%d addr %05x cnt %u%s) blksz %u blkcnt %u mode %04x\n", idx, arg, (arg >> 31) ? "W" : "R",
             (arg >> 28) & 7, (arg >> 9) & 0x1ffff, arg & 0x1ff, (arg >> 27) & 1 ? " blk" : "", HC16(0x04) & 0xfff, HC16(0x06), HC16(0x0c));
    switch (idx) {
    case 0: break;                                             /* GO_IDLE */
    case 3: resp = (uint32_t)rca << 16; break;                 /* SEND_RELATIVE_ADDR (R6) */
    case 5: resp = 0x80000000u | (2u << 28) | 0x00ff8000u; break; /* IO_SEND_OP_COND (R4): ready, 2 functions */
    case 7: resp = 0x00001e00; break;                          /* SELECT */
    case 52: {                                                 /* IO_RW_DIRECT */
        bool w = arg >> 31; int fn = (arg >> 28) & 7; bool raw = (arg >> 27) & 1;
        uint32_t addr = (arg >> 9) & 0x1ffff; uint8_t d = arg & 0xff, r = 0;
        if (w) {
            if (fn == 0) f0_write(addr, d); else if (fn == 1) f1_write(addr, d);
            if (raw) r = fn == 0 ? f0_read(addr) : f1_read(addr); else r = d;
        } else r = fn == 0 ? f0_read(addr) : fn == 1 ? f1_read(addr) : 0;
        resp = 0x1000 | r;
        break;
    }
    case 53: {                                                 /* IO_RW_EXTENDED */
        bool w = arg >> 31; bool blk = (arg >> 27) & 1;
        int cnt = arg & 0x1ff;
        int bsz = HC16(0x04) & 0xfff;
        int n = blk ? (cnt ? cnt : 512) * bsz : (cnt ? cnt : 512);
        if (n > (int)sizeof xfer) n = sizeof xfer;
        xfer_len = n; xfer_pos = 0; xfer_read = !w; xfer_arg = arg; xfer_active = true;
        xfer_blksz = blk ? bsz : n;
        xfer_blocks_left = blk ? (cnt ? cnt : 512) : 1;
        if (!w) cmd53_do_read();
        resp = 0x1000;
        break;
    }
    default:
        ok = false;                                            /* SD memory commands: no response */
        break;
    }
    if (!ok) { set_err(0x0001); return; }                      /* command timeout */
    HC32(0x10) = resp;
    set_int(0x0001);                                           /* command complete */
    if (data && xfer_active) {
        if (xfer_read) set_int(0x0020);                        /* buffer read ready */
        else set_int(0x0010);                                  /* buffer write ready */
    }
}

static void cmd53_do_read(void) {
    uint32_t arg = xfer_arg;
    int fn = (arg >> 28) & 7; bool inc = (arg >> 26) & 1;
    uint32_t addr = (arg >> 9) & 0x1ffff;
    if (fn == 1) f1_read_block(addr, xfer, xfer_len, inc);
    else if (fn == 2) f2_read_block(xfer, xfer_len);
    else for (int i = 0; i < xfer_len; i++) xfer[i] = f0_read(addr + (inc ? i : 0));
}
static void cmd53_do_write(void) {
    uint32_t arg = xfer_arg;
    int fn = (arg >> 28) & 7; bool inc = (arg >> 26) & 1;
    uint32_t addr = (arg >> 9) & 0x1ffff;
    if (fn == 1) f1_write_block(addr, xfer, xfer_len, inc);
    else if (fn == 2) f2_write_block(xfer, xfer_len);
    else for (int i = 0; i < xfer_len; i++) f0_write(addr + (inc ? i : 0), xfer[i]);
}

static void data_advance(int bytes) {
    int before = xfer_pos / xfer_blksz;
    xfer_pos += bytes;
    if (xfer_pos >= xfer_len) {
        if (!xfer_read) cmd53_do_write();
        xfer_active = false;
        set_int(0x0002);                                       /* transfer complete */
        return;
    }
    if (xfer_pos / xfer_blksz != before) set_int(xfer_read ? 0x0020 : 0x0010);
}

static uint32_t hc_read(void *ctx, uint32_t off, int size) {
    if (off >= 0x100) return 0;
    if (off == 0x20) {                                         /* buffer data port */
        uint32_t v = 0;
        if (xfer_active && xfer_read) {
            memcpy(&v, xfer + xfer_pos, size);
            data_advance(size);
        }
        return v;
    }
    if (off == 0x24) {                                         /* present state */
        uint32_t ps = 0x01ff0000;                              /* card inserted/stable/detect, DAT/CMD high */
        if (xfer_active) ps |= xfer_read ? 0x0a02 : 0x0502;
        HC32(0x24) = ps;
    }
    if (off == 0x2c || off == 0x2e) {
        if (HC16(0x2c) & 1) HC16(0x2c) |= 2;                  /* internal clock stable */
        hc[0x2f] = 0;                                          /* resets complete immediately */
    }
    if (off == 0x30 || off == 0x32) hc_irq_update();
    uint32_t v;
    if (size == 4) v = HC32(off & ~3u);
    else if (size == 2) v = HC16(off & ~1u);
    else v = hc[off];
    return v;
}

static void hc_write(void *ctx, uint32_t off, uint32_t val, int size) {
    if (off >= 0x100) return;
    if (off != 0x20) WLOG("[wifi] hc W%d +%02x <- %08x\n", size*8, off, val);
    if (off == 0x20) {
        WLOG("[wifi] dataport W%d %08x active=%d read=%d pos=%d/%d\n", size*8, val, xfer_active, xfer_read, xfer_pos, xfer_len);
        if (xfer_active && !xfer_read) {
            memcpy(xfer + xfer_pos, &val, size);
            data_advance(size);
        }
        return;
    }
    /* W1C interrupt status */
    if (off == 0x30 || off == 0x32) {
        if (size == 4) { HC16(0x30) &= ~(val & 0xfeff); HC16(0x32) &= ~(val >> 16); }
        else if (off == 0x30) HC16(0x30) &= ~(val & 0xfeff);
        else HC16(0x32) &= ~val;
        hc_irq_update();
        return;
    }
    if (size == 4) HC32(off & ~3u) = val;
    else if (size == 2) HC16(off & ~1u) = val;
    else hc[off] = val;
    if (off == 0x2f || (off == 0x2c && size == 4) || (off == 0x2e && size == 2)) {
        uint8_t r = hc[0x2f];
        if (r & 1) {                                           /* reset all */
            uint32_t caps = HC32(0x40), mc = HC32(0x48); uint16_t ver = HC16(0xfe);
            memset(hc, 0, sizeof hc);
            HC32(0x40) = caps; HC32(0x48) = mc; HC16(0xfe) = ver;
        }
        if (r & 6) { xfer_active = false; }
        hc[0x2f] = 0;
    }
    if ((off == 0x0e && size == 2) || (off == 0x0c && size == 4)) sd_command(HC16(0x0e));
    if (off >= 0x34 && off < 0x3c) hc_irq_update();
}

void wifi_tick(void) { if (chipram) dongle_tick(); }

void wifi_init(void) {
    chipram = calloc(1, RAM_BYTES);
    /* SDHCI capabilities: 50 MHz base/timeout clock, 1KB blocks, high speed, 3.3V, no DMA */
    HC32(0x40) = 0x01a132b2;
    HC32(0x48) = 0x00000040;
    HC16(0xfe) = 0x0001;            /* SDHCI spec 2.0 */
    /* CCCR */
    cccr[0x00] = 0x32;              /* CCCR rev 2, SDIO rev 3 */
    cccr[0x01] = 0x02;
    cccr[0x08] = 0x02;              /* supports multi-block */
    cccr[0x09] = 0x00; cccr[0x0a] = 0x10; cccr[0x0b] = 0x00;    /* common CIS at 0x1000 */
    cccr[0x13] = 0x01;              /* supports high speed */
    for (int f = 1; f <= 2; f++) {
        cccr[f * 0x100 + 0x00] = 0x00;
        uint32_t p = 0x1000 + f * 0x1000;
        cccr[f * 0x100 + 0x09] = p; cccr[f * 0x100 + 0x0a] = p >> 8; cccr[f * 0x100 + 0x0b] = p >> 16;
    }
    static const uint8_t ccis[] = { 0x20, 0x04, 0xd0, 0x02, 0x19, 0x43,  0x21, 0x02, 0x0c, 0x00,
                                    0x22, 0x04, 0x00, 0x00, 0x08, 0x32, 0xff };
    memcpy(cis, ccis, sizeof ccis);
    for (int f = 1; f <= 2; f++) {
        static const uint8_t fcis[] = { 0x20, 0x04, 0xd0, 0x02, 0x19, 0x43, 0xff };
        memcpy(cis + f * 0x1000, fcis, sizeof fcis);
    }
    bus_register(0xe0010000, 0x1000, "sdhci", NULL, hc_read, hc_write);
}
