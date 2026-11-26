/* wup_hw (see wup_hw.h): through the kernel (wupk.h) */
#include "wup_hw.h"
typedef uint8_t u8; typedef uint32_t u32;

/* ---- the backlight: the panel's own PWM (0..255), through the kernel. The six levels are those of the power chip's
   EEPROM table as measured on a pad (21 45 77 122 183 255): what the menu's older settings counted in */
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

/* ---- the rumble motor and the microphone: the kernel's (wupk.h; off again when the app ends) */
void hw_rumble(int on) { WUPK->rumble(on); }
void hw_mic(int on) { if (on) WUPK->mic_open(); else WUPK->mic_close(); }
int hw_mic_read(int16_t *out, int max) { return WUPK->mic_read(out, max); }
