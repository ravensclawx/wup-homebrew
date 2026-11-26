/* WPA2 passphrase -> PMK: PBKDF2-HMAC-SHA1, 4096 iterations, SSID as salt, 32 bytes. */
#include "wifi_hw.h"
#ifdef HOST_TEST
#define DLY(x) ((void)0)
#else
#include "wupk.h"
#define DLY(ms) WUPK->sleep(ms)
#endif
#define ROL(x, n) ((x) << (n) | (x) >> (32 - (n)))
static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        u32 t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static const u32 H0[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
/* hash (state after one 64-byte key block) || msg of mlen (< 56 bytes) */
static void sha1_finish(const u32 st[5], const u8 *msg, u32 mlen, u8 out[20]) {
    u32 h[5]; u8 blk[64];
    for (int i = 0; i < 5; i++) h[i] = st[i];
    for (u32 i = 0; i < 64; i++) blk[i] = 0;
    for (u32 i = 0; i < mlen; i++) blk[i] = msg[i];
    blk[mlen] = 0x80;
    u32 bits = (64 + mlen) * 8;
    blk[62] = bits >> 8; blk[63] = bits;
    sha1_block(h, blk);
    for (int i = 0; i < 5; i++) { out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i]; }
}
void wpa_pmk(const char *pass, const u8 *ssid, u32 ssid_len, u8 out[32]) {
    u8 k[64], blk[64]; u32 ist[5], ost[5], plen = 0;
    while (pass[plen] && plen < 63) plen++;
    for (int i = 0; i < 64; i++) k[i] = i < (int)plen ? (u8)pass[i] : 0;
    for (int i = 0; i < 5; i++) ist[i] = ost[i] = H0[i];
    for (int i = 0; i < 64; i++) blk[i] = k[i] ^ 0x36; sha1_block(ist, blk);
    for (int i = 0; i < 64; i++) blk[i] = k[i] ^ 0x5c; sha1_block(ost, blk);
    for (u32 b = 1; b <= 2; b++) {
        u8 s[40], u[20], t[20], x[20];
        for (u32 i = 0; i < ssid_len; i++) s[i] = ssid[i];
        s[ssid_len] = 0; s[ssid_len + 1] = 0; s[ssid_len + 2] = 0; s[ssid_len + 3] = (u8)b;
        sha1_finish(ist, s, ssid_len + 4, x); sha1_finish(ost, x, 20, u);
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (int it = 1; it < 4096; it++) {
            sha1_finish(ist, u, 20, x); sha1_finish(ost, x, 20, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
            if (!(it & 511)) DLY(1);                   /* (the other tasks a turn) */
        }
        for (int i = 0; i < 20 && (b - 1) * 20 + i < 32; i++) out[(b - 1) * 20 + i] = t[i];
    }
    for (int i = 0; i < 64; i++) k[i] = blk[i] = 0;
}
