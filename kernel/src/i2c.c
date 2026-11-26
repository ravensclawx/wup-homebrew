/* I2C: the Renesas-style controller, channel 3 (0xF0006400), the way LVC's driver uses it (LVC 0xa25c-0xb4c6):
 *   global 0xF0005800: +0x00 pending (bit per channel), +0x04 interrupt enable, +0x08 write 1 to clear
 *   channel: +0x00 enable, +0x04 data, +0x08 command (bit 0 STOP, bit 1 START, 0x18 the base value),
 *            +0x10 clock (0x0C), +0x18 status (bit 2 ACK received), +0x1C error, +0x20 bus state (3 idle)
 * Every step raises the channel's pending bit; this polls it (interrupt 15 stays masked).
 * Devices on channel 3: 0x39 the LCD panel's driver IC (Renesas R69208), 0x21 the camera, 0x18 the audio codec. */
#include "os.h"

#define G(o)   REG(0xf0005800u + (o))
#define C(o)   REG(0xf0006400u + (o))
#define BIT    8u                                         /* channel 3 */

void i2c_init(void) {
    G(0x04) = 0;
    G(0x08) = 0x1c;                                       /* (LVC clears channels 2-4) */
    C(0x10) = 0x0c;
    C(0x20) = 3;
    C(0x00) = 1;
}

static int step(void) {
    for (u32 t = time_us(); !(G(0x00) & BIT); ) if (time_us() - t > 5000) return -1;
    u32 st = C(0x18);
    G(0x08) = BIT;
    return st;
}

int i2c_write(int addr, const u8 *p, int n) {
    int err = 0, st;
    C(0x08) = 0x18;
    G(0x04) |= BIT;
    /* bus idle: 3 after the channel's set-up, 1 after a STOP (the pad, 2026-11-19: it never reads 3 again) */
    for (u32 t = time_us(); !(C(0x20) & 1); ) if (time_us() - t > 5000) {
        klog("i2c: bus state %x, status %x: channel reset\n", C(0x20), C(0x18));
        i2c_init();
        break;
    }
    C(0x08) = 0x1a;                                       /* START */
    C(0x04) = (u32)addr << 1;
    st = step();
    if (st < 0 || !(st & 4)) err = -2;                    /* no ACK */
    for (int i = 0; i < n && !err; i++) {
        C(0x04) = p[i];
        st = step();
        if (st < 0 || !(st & 4)) err = -3;
    }
    C(0x08) = 0x19;                                       /* STOP */
    step();
    G(0x04) &= ~BIT;
    return err;
}

/* the address phase alone to `addr`, then STOP (is the device there?). Returns the status word (bit 2: ACK), or -1 when
   nothing came back; *err gets the error register (+0x1C), *bus the bus state (+0x20) first */
int i2c_probe(int addr, u32 *err, u32 *bus) {
    int st;
    C(0x08) = 0x18;
    G(0x04) |= BIT;
    *bus = C(0x20);
    C(0x08) = 0x1a;
    C(0x04) = (u32)addr << 1;
    st = step();
    *err = C(0x1c);
    C(0x08) = 0x19;
    step();
    G(0x04) &= ~BIT;
    return st;
}

/* register `reg` of device `addr`: the register byte written, a repeated START, then n bytes read, the way LVC reads
   the panel's ID (LVC 0xb688-0xb8aa): +0x08 0x14 to receive, 0x34 per byte with ACK, 0x10 then 0x38 for the last
   (no ACK), then STOP */
int i2c_read(int addr, u8 reg, u8 *p, int n) {
    int err = 0, st;
    C(0x08) = 0x18;
    G(0x04) |= BIT;
    C(0x08) = 0x1a;
    C(0x04) = (u32)addr << 1;
    st = step(); if (st < 0 || !(st & 4)) err = -2;
    if (!err) { C(0x04) = reg; st = step(); if (st < 0 || !(st & 4)) err = -3; }
    if (!err) {
        C(0x08) = 0x1a;                                   /* repeated START */
        C(0x04) = (u32)addr << 1 | 1;
        st = step(); if (st < 0 || !(st & 4)) err = -4;
    }
    if (!err) {
        C(0x08) = 0x14;
        for (int i = 0; i < n && !err; i++) {
            if (i == n - 1) { C(0x08) = 0x10; C(0x08) = 0x38; }
            else C(0x08) = 0x34;
            if (step() < 0) err = -5;
            else p[i] = (u8)C(0x04);
        }
    }
    C(0x08) = 0x19;                                       /* STOP */
    step();
    G(0x04) &= ~BIT;
    return err;
}
