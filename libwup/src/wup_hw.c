/* wup_hw (see wup_hw.h). Every address is from the stock firmware (LVC rev 7677) and was checked on the real pad
 * (wupstream: the backlight, the motor and the microphone driven from a PC). */
#include "wup_hw.h"
typedef uint8_t u8; typedef uint32_t u32;
#define REG(a) (*(volatile u32 *)(a))

/* ---- the backlight. Set as the firmware's "set brightness" does (console command 5/0x14, LVC 0x34900 -> 0x32698):
   it hands {0, PWM level 0..255, frequency} to the LCD driver (LVC 0xdc7c, Thumb; its handle at 0xbb754). The console
   only ever picks one of the six {PWM, frequency} pairs of the firmware's copy of the power chip's EEPROM table
   (0xbdc34; on this pad 21 45 77 122 183 255, frequency 1 in every pair), but the driver takes any PWM level. */
#ifdef WUP_KERNEL
/* (the wup-homebrew kernel: the panel's own PWM; the six levels of the power chip's table as measured on a pad) */
#include "wupk.h"
static const u8 levels[6] = { 21, 45, 77, 122, 183, 255 };
void hw_backlight(int percent) { if (percent >= 0 && percent <= 100) WUPK->backlight((percent * 255 + 50) / 100); }
int hw_backlight_percent(int level) { return level >= 1 && level <= 6 ? ((levels[level - 1] * 100 / 255 + 5) / 10) * 10 : 0; }
int hw_backlight_default(void) {                         /* the EEPROM's own level: UIC EEPROM 0x1286 {level, CRC-16} */
    u8 q[4] = { 0x03, 0x12, 0x86, 3 }, b[3];
    if (WUPK->uic(q, 4, b, 3)) return -1;
    u32 c = 0xffff ^ b[0];
    for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0x8408 : c >> 1;
    return b[0] >= 1 && b[0] <= 5 && c == (u32)(b[1] | b[2] << 8) ? hw_backlight_percent(b[0]) : -1;
}
void hw_screen_power(int on) { u8 c[2] = { 0x12, (u8)!!on }; WUPK->uic(c, 2, 0, 0); }
#else
#define TABLE ((const volatile u8 *)0xbdc34)
void hw_backlight(int percent) {
    static u8 blk[4];
    if (percent < 0 || percent > 100) return;
    blk[0] = 0; blk[1] = (u8)((percent * 255 + 50) / 100); blk[2] = TABLE[2 * 5 + 1];   /* (the top pair's frequency) */
    ((int (*)(u32, u8 *))0x0000dc7d)(*(volatile u32 *)0xbb754, blk);
}
int hw_backlight_percent(int level) { return level >= 1 && level <= 6 ? ((TABLE[2 * (level - 1)] * 100 / 255 + 5) / 10) * 10 : 0; }
/* the EEPROM's own level (config 0x286 = {level, CRC-16}: what the console and Nintendo's mode use), only read; as a
   percentage, to the nearest 10 */
int hw_backlight_default(void) {
    static u8 b[4];
    volatile u32 *uc = (volatile u32 *)0xbb7d0;           /* the firmware's EEPROM handle (LVC 0x3c97e; opened on first use) */
    if (!*uc && ((int (*)(volatile u32 *))0x00011de1)(uc)) return 0;
    if (((int (*)(u32, u32, u8 *, u32))0x000129fd)(*uc, 0x286, b, 3)) return 0;
    u32 c = 0xffff ^ b[0];                                /* (CRC-16, 0x8408) */
    for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0x8408 : c >> 1;
    return b[0] >= 1 && b[0] <= 5 && c == (u32)(b[1] | b[2] << 8) ? hw_backlight_percent(b[0]) : -1;
}
/* the power chip's command 0x12 0 / 1, through the firmware's function (LVC 0x13142, Thumb; its handle at 0xbb2d0) */
void hw_screen_power(int on) { ((int (*)(u32, u32))0x00013143)(*(volatile u32 *)0xbb2d0, (u32)!!on); }
#endif

/* ---- the rumble motor: pin 4 of the vibrate module's GPIO table, bit 8 of 0xF0005114 (read from the running pad) */
void hw_rumble(int on) { if (on) REG(0xf0005114u) |= 0x100; else REG(0xf0005114u) &= ~0x100u; }

/* ---- the microphone: the capture block (0xF0005480; the firmware's driver is drvacapt, LVC 0x2000-0x2e00). The
   firmware sets it up at boot (16-bit mono, 48 kHz; a ring of 0xC00 bytes: +0x20 start, +0x24 end, +0x28 how far the
   driver has read, +0x2c how far into the ring it has written, all in 16-byte units) but only runs it for the console.
   Started as its start routine does (LVC 0x256a): a reset pulse (bit 31), the read position to the ring's start,
   bit 0 = run. The ring holds 32 ms: read it at least every 20 ms. */
#define CAP(o)  REG(0xf0005480u + (o))
#define CAPCTL  REG(0xf0005444u)
static u32 mic_on, mic_rd;
void hw_mic(int on) {
    if (!!on == (int)mic_on) return;
    if (on) { CAPCTL |= 0x80000000u; CAPCTL &= ~0x80000000u; CAP(0x28) = CAP(0x20); mic_rd = CAP(0x20) << 4; CAPCTL |= 1; }
    else CAPCTL &= ~1u;
    mic_on = (u32)!!on;
}
int hw_mic_read(int16_t *out, int max) {
    if (!mic_on) return 0;
    u32 a = CAP(0x20) << 4, e = CAP(0x24) << 4, w = a + (CAP(0x2c) << 4), got = 0;
    if (w >= e || mic_rd < a || mic_rd >= e) return 0;
    while (mic_rd != w && got < (u32)max) {
        u32 n = ((w > mic_rd ? w : e) - mic_rd) / 2;
        if (n > (u32)max - got) n = (u32)max - got;
        const volatile int16_t *s = (const volatile int16_t *)mic_rd;
        for (u32 i = 0; i < n; i++) out[got + i] = s[i];
        got += n; mic_rd += n * 2;
        if (mic_rd == e) mic_rd = a;
    }
    return (int)got;
}
