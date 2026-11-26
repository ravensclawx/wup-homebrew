/* The Wi-Fi radio's firmware, straight from Nintendo's firmware bank in flash: the streamer starts from the Homebrew
 * Menu's hook, so Nintendo's firmware is always there, and nothing of it is stored again or shipped.
 *
 * The radio (Broadcom BCM4319) runs Broadcom's firmware, which the pad downloads into it at every start: the WIFI
 * section of the running bank (after its 32-byte header), and the NVRAM text (the board's settings): its own WNVR
 * section where the bank has one, else inside LVC_ ("nvramver=" ... a blank line). The bank is the one the selector
 * byte (flash 0xF000) boots: 1 = Product2 (0x500000), anything else Product1 (0x100000).
 *
 * Two changes make it join ordinary networks, applied to the bytes as they are read (wifi.c downloads it in pieces):
 * its WPA2 supplicant accepts only Nintendo's OUI (A4:C0:E1), switched to the standard 00:0F:AC in the four places
 * that test it; and it rotates the pairwise key 3 bytes after deriving it, which is undone. Every place is checked
 * byte by byte first; then the CRC-32 the image carries over itself (image 0x3D300) is the patched image's, or the
 * radio's boot ROM will not start it.
 *
 * wifi.c reads a package: "WFW1", u32 firmware length, u32 NVRAM length, the firmware, the NVRAM ('\n' -> '\0'). */
#include "os.h"

static const struct { u32 at; u8 n; u8 old[4], new_[4]; } patch[] = {      /* (offsets in the image) */
    { 0x297f2, 3, { 0xa4, 0xc0, 0xe1 }, { 0x00, 0x0f, 0xac } },              /* the OUI checks */
    { 0x2afef, 3, { 0xa4, 0xc0, 0xe1 }, { 0x00, 0x0f, 0xac } },
    { 0x2aff5, 3, { 0xa4, 0xc0, 0xe1 }, { 0x00, 0x0f, 0xac } },
    { 0x2affb, 3, { 0xa4, 0xc0, 0xe1 }, { 0x00, 0x0f, 0xac } },
    { 0x24c1c, 2, { 0xe1, 0x1c }, { 0x21, 0x1c } },                          /* adds r1, r4, #3 -> #0 */
    { 0x24c1e, 4, { 0xaa, 0xf1, 0x03, 0x02 }, { 0xaa, 0xf1, 0x00, 0x02 } },  /* sub.w r2, r10, #3 -> #0 */
    { 0x24c30, 4, { 0xdc, 0xf7, 0xbe, 0xfd }, { 0x00, 0xbf, 0x00, 0xbf } },  /* bl memcpy -> nop nop */
};
#define NPATCH (sizeof patch / sizeof patch[0])
#define CRC_AT 0x3d300u                          /* the image's own CRC-32, over image 0x0..0x3D300 */
#define CRC_STOCK 0x8a7f51a5u                    /* (firmware 7677's) */

static u32 img_at, img_len, nv_len, crc_new;
static u8 nv[4096] __attribute__((aligned(4)));
static int state;                                /* 0 not looked at, 1 ready, < 0 not usable */

static int section(u32 bank, const char *tag, u32 *at, u32 *size) {
    u32 idx[64];
    if (flash_read(bank, idx, sizeof idx) || memcmp((u8 *)idx + 8, "INDX", 4)) return -1;
    for (int i = 0; i < 64; i += 4)
        if (!memcmp(&idx[i + 2], tag, 4) && idx[i + 1]) { *at = bank + idx[i]; *size = idx[i + 1]; return 0; }
    return -1;
}

/* the NVRAM text: whole in WNVR, or the part of LVC_ from "nvramver=" to a blank line (looked for from late in it,
   where firmware 7677 keeps it, then from its start) */
static int nvram(u32 bank) {
    u32 at, size;
    static u8 b[4096 + 4096];
    if (!section(bank, "WNVR", &at, &size) && size < sizeof nv) {
        if (flash_read(at, nv, size)) return -1;
        nv_len = size;
    } else {
        if (section(bank, "LVC_", &at, &size)) return -1;
        u32 starts[2] = { size > 0xa0000 ? 0xa0000 : 0, 0 };
        nv_len = 0;
        for (int pass = 0; pass < 2 && !nv_len; pass++) {
            for (u32 o = starts[pass]; o < size && !nv_len; o += 4096) {
                u32 k = size - o < sizeof b ? size - o : sizeof b;      /* (two pieces: the text may cross one) */
                if (flash_read(at + o, b, k)) return -1;
                for (u32 i = 0; i + 9 <= (k < 4096 ? k : 4096); i++) {
                    if (b[i] != 'n' || memcmp(b + i, "nvramver=", 9)) continue;
                    u32 e = i;
                    while (e + 1 < k && !(b[e] == '\n' && b[e + 1] == '\n')) e++;
                    if (e + 1 >= k || e + 2 - i > sizeof nv) return -1;
                    nv_len = e + 2 - i;
                    memcpy(nv, b + i, nv_len);
                    break;
                }
            }
            if (!starts[pass]) break;
        }
        if (!nv_len) return -1;
    }
    for (u32 i = 0; i < nv_len; i++) if (nv[i] == '\n') nv[i] = 0;
    while (nv_len % 4) nv[nv_len++] = 0;
    return 0;
}

static int prepare(void) {
    if (state) return state > 0 ? 0 : state;
    state = -1;
    u8 sel = 0;
    flash_read(0xf000, &sel, 1);
    u32 bank = sel == 1 ? 0x500000u : 0x100000u, at, size;
    if (section(bank, "WIFI", &at, &size) || size < CRC_AT + 0x24) { klog("radio: no WIFI section in the bank at %x\n", bank); return -1; }
    img_at = at + 0x20; img_len = size - 0x20;
    for (u32 i = 0; i < NPATCH; i++) {
        u8 b[4];
        if (flash_read(img_at + patch[i].at, b, patch[i].n) || memcmp(b, patch[i].old, patch[i].n)) {
            klog("radio: image +%x is not as expected: a different radio firmware\n", patch[i].at); return -2;
        }
    }
    u32 crc;
    if (flash_read(img_at + CRC_AT, &crc, 4) || crc != CRC_STOCK) { klog("radio: unexpected image CRC %08x\n", crc); return -3; }
    /* the patched image's CRC: read once through radio_read (which patches), the CRC word itself left out */
    static u8 b[4096];
    crc_new = 0;
    state = 2;                                   /* (radio_read may run now; the CRC word is not asked for here) */
    for (u32 o = 0; o < CRC_AT; o += sizeof b) {
        u32 k = CRC_AT - o < sizeof b ? CRC_AT - o : sizeof b;
        if (radio_read(12 + o, b, k)) { state = -4; return -4; }
        crc_new = crc32_more(crc_new, b, k);
    }
    if (nvram(bank)) { klog("radio: no NVRAM text in the bank\n"); state = -5; return -5; }
    state = 1;
    klog("radio: firmware %u bytes and NVRAM %u bytes from the bank at %x, patched (CRC %08x)\n", img_len, nv_len, bank, crc_new);
    return 0;
}

int radio_size(void) { return prepare() ? -1 : (int)(12 + img_len + nv_len); }

int radio_read(u32 off, void *p, u32 n) {
    if (state != 2 && prepare()) return -1;
    u8 *d = p;
    while (n) {
        if (off < 12) {                                          /* the package's header */
            u32 h[3] = { 0x31574657u, img_len, nv_len };          /* "WFW1" */
            u32 k = 12 - off < n ? 12 - off : n;
            memcpy(d, (u8 *)h + off, k); d += k; off += k; n -= k;
        } else if (off < 12 + img_len) {                         /* the image, patched */
            u32 o = off - 12, k = img_len - o < n ? img_len - o : n;
            if (flash_read(img_at + o, d, k)) return -1;
            for (u32 i = 0; i < NPATCH; i++)
                for (u32 j = 0; j < patch[i].n; j++)
                    if (patch[i].at + j >= o && patch[i].at + j < o + k) d[patch[i].at + j - o] = patch[i].new_[j];
            for (u32 j = 0; j < 4; j++)
                if (state == 1 && CRC_AT + j >= o && CRC_AT + j < o + k) d[CRC_AT + j - o] = (u8)(crc_new >> (8 * j));
            d += k; off += k; n -= k;
        } else {                                                 /* the NVRAM */
            u32 o = off - 12 - img_len;
            if (o + n > nv_len) return -1;
            memcpy(d, nv + o, n); n = 0;
        }
    }
    return 0;
}
