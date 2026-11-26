/* The UIC: the Renesas microcontroller that owns the buttons, sticks, touch, sensors, battery and power, on the SPI
 * bus (spi.c; chip select 0xF00050FC). Commands (as LVC sends them): 0x07 input report (128 bytes), 0x0B UIC version
 * (4), 0x10 battery 0..6, 0x12 xx LCD power, 0x13 the SoC is up, 0x03 AH AL N read EEPROM, 0x01 xx restart in
 * mode xx (0x04: power off). */
#include "os.h"

#define UIC_CS 0xf00050fcu

u8 uic_report[128];

int uic_cmd(const u8 *out, int nout, u8 *in, int nin) {
    return spi_xfer(UIC_CS, out, (u32)nout, NULL, 0, in, (u32)nin);
}

/* the input report: 128 bytes. On the pad (2026-11-19): byte 0 is constant (0x28), byte 1 counts, byte 127 is the
   complement of byte 0 (the check); sticks at 6..13 (12 bits, around 0x800), volume at 14, as the stock firmware reads them */
int uic_poll(void) {
    static const u8 c = 0x07;
    u8 r[128];
    if (uic_cmd(&c, 1, r, 128)) return -1;
    if ((u8)(r[0] ^ 0xff) != r[127]) return -2;          /* (the first reply after start-up is all zeros) */
    memcpy(uic_report, r, 128);
    return 0;
}

/* LCD power: LVC sends 12 00 with the panel's "display off", 12 01 after its "display on" */
void uic_lcd_power(int on) {
    u8 c[2] = { 0x12, (u8)(on ? 1 : 0) };
    uic_cmd(c, 2, NULL, 0);
}
