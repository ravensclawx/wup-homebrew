/* fwpatch: see fwpatch.h.
 *
 * drc_fw.bin: a big-endian header {image version, block size, sequences per session, image size}, then the image:
 *   0x0000  header (little-endian): type, 4 super CRCs (each over 0x1000 bytes of the sub CRC table), padding, CRC of
 *           the first 0xFFC bytes
 *   0x1000  sub CRC table: 0x1000 CRC-32s, one per 0x1000 bytes of the data (the last over what is left); 0 unused
 *   0x5000  the data: the sections (INDX, VER_, LVC_, WIFI, ERR_, UMI_, IMG_), as they lie in the pad's firmware bank
 * (the layout as DRXUtil reads and writes it: github.com/GaryOderNichts/DRXUtil, source/Firmware.cpp).
 *
 * The change, the same as the UART install's in Product1 (wup-menu: tools/mkflash.py): the early hook's two pieces over
 * unused strings in LVC_ and the branch to it at LVC_ 0x40, and the bundle (the kernel and the menu, as their slots
 * store them) after the last section at BUNDLE_OFF, where the early hook looks in the bank that runs (boot/early.c).
 * Nothing else changes; the version stays. */
#include <stdlib.h>
#include <string.h>
#include "fwpatch.h"

#define HDR 16u
#define FWHDR 0x1000u
#define SUBCRC 0x4000u
#define DATA (FWHDR + SUBCRC)
#define BLOCK 0x1000u

/* the payload (wup-menu: tools/mkinstaller.py), little-endian: a head, {offset in LVC_, length} per patch, the
   patches' bytes, the bundle */
enum { P_MAGIC, P_VERSION, P_FWVER, P_LVC_SIZE, P_LVC_CRC, P_BUNDLE_OFF, P_NPATCH, P_BUNDLE_LEN, P_COUNT };
#define PAYLOAD_MAGIC 0x49505557u   /* "WUPI" */
#define NPATCH_MAX 8

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

uint32_t fwpatch_crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xffffffffu;
    while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

typedef struct { uint32_t v[P_COUNT], at[NPATCH_MAX], len[NPATCH_MAX]; const uint8_t *data[NPATCH_MAX], *bundle; } payload_t;
static int read_payload(const uint8_t *p, size_t n, payload_t *pl, const char **err) {
    *err = "Damaged installer";
    if (n < 4 * P_COUNT) return -1;
    for (int i = 0; i < P_COUNT; i++) pl->v[i] = le32(p + 4 * i);
    uint32_t np = pl->v[P_NPATCH];
    if (pl->v[P_MAGIC] != PAYLOAD_MAGIC || pl->v[P_VERSION] != 2 || np > NPATCH_MAX || n < 4 * P_COUNT + 8 * np) return -1;
    const uint8_t *d = p + 4 * P_COUNT + 8 * np;
    uint64_t total = 4 * P_COUNT + 8 * np;
    for (uint32_t i = 0; i < np; i++) {
        pl->at[i] = le32(p + 4 * P_COUNT + 8 * i); pl->len[i] = le32(p + 4 * P_COUNT + 8 * i + 4);
        if ((uint64_t)pl->at[i] + pl->len[i] > pl->v[P_LVC_SIZE]) return -1;
        pl->data[i] = d; d += pl->len[i]; total += pl->len[i];
    }
    if (total + pl->v[P_BUNDLE_LEN] != n) return -1;
    pl->bundle = d;
    return 0;
}

/* -> the data's length, and LVC_'s offset in it */
static int check(const uint8_t *o, size_t n, const payload_t *pl, size_t *dlen, size_t *lvc, const char **err) {
    if (n < HDR + DATA + 0x70) { *err = "Unknown GamePad firmware"; return -1; }
    uint32_t ver = be32(o), size = be32(o + 12);
    if (size != n - HDR || size < DATA) { *err = "Unknown GamePad firmware"; return -1; }
    if (ver != pl->v[P_FWVER]) { *err = "Unsupported GamePad firmware version"; return -1; }
    const uint8_t *h = o + HDR, *sub = h + FWHDR, *d = h + DATA;
    *dlen = size - DATA;
    if (le32(h + 0xffc) != fwpatch_crc32(h, 0xffc)) { *err = "Damaged GamePad firmware"; return -1; }
    for (int i = 0; i < 4; i++) if (le32(h + 4 + 4 * i) != fwpatch_crc32(sub + i * BLOCK, BLOCK)) { *err = "Damaged GamePad firmware"; return -1; }
    if (*dlen > (size_t)BLOCK * (SUBCRC / 4)) { *err = "Unknown GamePad firmware"; return -1; }
    for (size_t i = 0, k = 0; i < *dlen; i += BLOCK, k++) {
        size_t m = *dlen - i < BLOCK ? *dlen - i : BLOCK;
        if (le32(sub + 4 * k) != fwpatch_crc32(d + i, m)) { *err = "Damaged GamePad firmware"; return -1; }
    }
    /* INDX: {offset, size, name, version} per section, itself first */
    if (memcmp(d + 8, "INDX", 4) || le32(d) != 0) { *err = "Unknown GamePad firmware"; return -1; }
    uint32_t nsec = le32(d + 4) / 16;
    if (nsec * 16 > *dlen) { *err = "Unknown GamePad firmware"; return -1; }
    *lvc = 0;
    for (uint32_t i = 1; i < nsec; i++) {
        const uint8_t *e = d + 16 * i;
        uint32_t off = le32(e), sz = le32(e + 4);
        if ((uint64_t)off + sz > *dlen) { *err = "Unknown GamePad firmware"; return -1; }
        if (!memcmp(e + 8, "LVC_", 4)) {
            if (sz != pl->v[P_LVC_SIZE] || fwpatch_crc32(d + off, sz) != pl->v[P_LVC_CRC]) { *err = "Unsupported GamePad firmware version"; return -1; }
            *lvc = off;
        }
    }
    if (!*lvc) { *err = "Unknown GamePad firmware"; return -1; }
    if (*dlen > pl->v[P_BUNDLE_OFF]) { *err = "Unsupported GamePad firmware version"; return -1; }
    return 0;
}

int fwpatch_check(const uint8_t *orig, size_t orig_len, const uint8_t *payload, size_t payload_len, const char **err) {
    payload_t pl; size_t dlen, lvc;
    if (read_payload(payload, payload_len, &pl, err)) return -1;
    return check(orig, orig_len, &pl, &dlen, &lvc, err);
}

int fwpatch(const uint8_t *orig, size_t orig_len, const uint8_t *payload, size_t payload_len,
            uint8_t **out, size_t *out_len, const char **err) {
    payload_t pl; size_t dlen, lvc;
    *out = NULL; *out_len = 0;
    if (read_payload(payload, payload_len, &pl, err) || check(orig, orig_len, &pl, &dlen, &lvc, err)) return -1;
    size_t nlen = (size_t)pl.v[P_BUNDLE_OFF] + pl.v[P_BUNDLE_LEN];
    if (nlen > (size_t)BLOCK * (SUBCRC / 4)) { *err = "Damaged installer"; return -1; }
    uint8_t *b = malloc(HDR + DATA + nlen);
    if (!b) { *err = "Out of memory"; return -1; }
    uint8_t *h = b + HDR, *sub = h + FWHDR, *d = h + DATA;
    memcpy(b, orig, HDR + FWHDR);                         /* the headers as they were (CRCs and size below) */
    memcpy(d, orig + HDR + DATA, dlen);
    memset(d + dlen, 0xff, pl.v[P_BUNDLE_OFF] - dlen);     /* (erased flash) */
    memcpy(d + pl.v[P_BUNDLE_OFF], pl.bundle, pl.v[P_BUNDLE_LEN]);
    for (uint32_t i = 0; i < pl.v[P_NPATCH]; i++) memcpy(d + lvc + pl.at[i], pl.data[i], pl.len[i]);
    memset(sub, 0, SUBCRC);
    for (size_t i = 0, k = 0; i < nlen; i += BLOCK, k++)
        put_le32(sub + 4 * k, fwpatch_crc32(d + i, nlen - i < BLOCK ? nlen - i : BLOCK));
    for (int i = 0; i < 4; i++) put_le32(h + 4 + 4 * i, fwpatch_crc32(sub + i * BLOCK, BLOCK));
    put_le32(h + 0xffc, fwpatch_crc32(h, 0xffc));
    put_be32(b + 12, (uint32_t)(DATA + nlen));
    *out = b; *out_len = HDR + DATA + nlen;
    return 0;
}
