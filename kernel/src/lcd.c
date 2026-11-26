/* The display: the stock firmware's display bring-up replayed (fw_init.c), then our framebuffer on the UI layer and
 * the panel switched on the way LVC does it.
 *
 * LCD controller (0xF0009400), the UI layer: +0x60 x, +0x68 y, +0x64 width, +0x6C height, +0x70 stride, +0x74
 * address, +0xB0 format (bits 0-1: 3 = RGB565 with the stride in pixels, as an earlier homebrew ran it on the pad; the firmware's
 * own 0x00FF0008 is 8-bit paletted), +0x80 bit 4 the layer shown, +0x200 the gamma curve (33 points, 0..256),
 * +0x284 the channels it applies to. Interrupt 22 at each vertical blank.
 *
 * The panel (Renesas R69208 on I2C channel 3, address 0x39; LVC "[LCD] JDI R69208 Initialize"): 0xB0 02 / 0xB0 03
 * open and close its manufacturer commands, 0xBB 08 PWM 01 00 sets the backlight (PWM 0..255; LVC's boot value 0x7A),
 * 0xB8 the backlight control, 0x28 / 0x29 display off / on (MIPI DCS). The UIC switches the LCD's power (12 xx). */
#include "os.h"
#include "fw_init.h"

#define LCD(o) REG(0xf0009400u + (o))

volatile u32 vblanks, vblank_us;                         /* (vblank_us: time_us() at the last one) */
static void vblank(int n) { (void)n; vblanks++; vblank_us = time_us(); }

static void panel(const u8 *p, int n) {
    int e = i2c_write(0x39, p, n);
    if (e) klog("lcd: panel command %02x failed (%d)\n", p[0], e);
}
static void panel_backlight(int pwm) {
    static const u8 open[] = { 0xb0, 0x02 }, close[] = { 0xb0, 0x03 };
    u8 bb[] = { 0xbb, 0x08, (u8)pwm, 0x01, 0x00 };
    panel(open, 2); panel(bb, 5); panel(close, 2);
}

/* a pin as an output (0xC200 low, 0xC300 high): LVC's GPIO driver, 0xa0b0 (configure low) and 0xa1aa (set) */
static void pin_pulse(u32 reg, u32 low_ms) {
    REG(reg) = 0xc200; REG(reg) = 0xc200;
    delay_ms(low_ms);
    REG(reg) = 0xc300;
}

/* in LVC's order (the stock boot, 0.30-0.56 s; the emulator I2C_LOG / MMIOTRACE) */
void lcd_init(int pwm, int panel_setup) {
    static const u8 open[] = { 0xb0, 0x02 }, close[] = { 0xb0, 0x03 }, off[] = { 0x28 }, on[] = { 0x29 };
    static const u8 b8[] = { 0xb8, 0x00, 0x18, 0x01, 0x04, 0x40 };

    fw_replay(fw_display, fw_display_n);

    LCD(0x74) = FB0_ADDR;                                 /* the UI layer, as LVC 0x174a2 sets its own */
    LCD(0x60) = 0x60;
    LCD(0x68) = 8;
    LCD(0x64) = SCREEN_W;
    LCD(0x6c) = SCREEN_H;
    LCD(0x70) = SCREEN_W;
    LCD(0xb0) = 0x00ff0008u | 3;
    LCD(0xb4) = 0;
    for (u32 i = 0; i <= 32; i++) LCD(0x200 + 4 * i) = i * 8;   /* gamma: straight */
    LCD(0x284) = 7;
    irq_attach(22, 4, vblank);

    if (!panel_setup) { LCD(0x80) |= 0x10; return; }

    /* The panel as the stock firmware starts it (traced in the emulator, LVC 7677): out of reset at 0.305 s, its ID
       read and the backlight set at 0.32 s, display off at 0.432 s and at once the LCD's power off (UIC 12 00), then
       ~25 ms later the backlight, its settings (B8), display on and the LCD's power on (12 01); the picture layer at
       0.605 s. Without that power cycle the panel showed a line at every power-on (the pad, 2026-11-26). In a normal
       boot the UIC has the LCD powered already; booted over UART nothing has, and the panel then does not answer
       until 12 01 and 100 ms (the pad, 2026-11-19): that only as a fallback. */
    pin_pulse(0xf0005100u, 5);                            /* the panel out of reset (LVC: 0.300 s low, 0.305 s high) */
    delay_ms(15);
    u32 err, bus; int st = i2c_probe(0x39, &err, &bus);
    if (st < 0 || !(st & 4)) {
        uic_lcd_power(1); delay_ms(100);
        pin_pulse(0xf0005100u, 5); delay_ms(15);
        st = i2c_probe(0x39, &err, &bus);
    }
    klog("lcd: panel %s (status %x)\n", st >= 0 && (st & 4) ? "answers" : "does not answer", (u32)st);
    panel_backlight(pwm);                                 /* "[LCD] JDI R69208 Initialize" (LVC 0.32 s) */
    pin_pulse(0xf000511cu, 5);                            /* LVC 0.322 s (the camera's reset, inferred) */
    REG(0xf0005114u) = 0xc200; REG(0xf0005118u) = 0xc200; /* LVC 0.338 s: rumble off, and its neighbour */
    delay_ms(90);                                         /* (LVC sets the camera up here: display off at 0.432 s) */
    panel(off, 1);
    uic_lcd_power(0);                                     /* LVC 0.433 s: the LCD's power off ... */
    delay_ms(25);                                         /* (LVC sets the sound codec up meanwhile) */
    panel_backlight(pwm);
    panel(open, 2); panel(b8, 6); panel(close, 2);
    panel(on, 1);
    uic_lcd_power(1);                                     /* ... and on again after the display-on (LVC ~0.46 s) */
    for (int i = 0; i < 4; i++) lcd_wait_vblank();
    LCD(0x80) |= 0x10;                                    /* the picture layer last (LVC 0.605 s) */
}

void lcd_backlight(int pwm) { panel_backlight(pwm); }

/* the screen off the way the stock firmware turns the pad off (LVC; the UIC's 12 00 last, in the emulator's trace of
   its power-off): black, the panel's display off and asleep, then the LCD's power. A panel that loses its power with
   a picture on it keeps a ghost of that picture into the next power-on (seen on the pad 2026-11-26) */
void lcd_off(void) {
    static const u8 off[] = { 0x28 }, sleep_in[] = { 0x10 };
    u32 top = LCD(0x200 + 4 * 32) & 0x1ff;                /* the brightness now (256: full), faded to black in 18 frames */
    for (int f = 17; f >= 0; f--) {
        for (u32 i = 0; i <= 32; i++) LCD(0x200 + 4 * i) = (i * 8 * top / 256) * (u32)f / 18;
        lcd_wait_vblank();
    }
    lcd_wait_vblank();
    panel(off, 1);
    lcd_wait_vblank(); lcd_wait_vblank();
    panel(sleep_in, 1);
    klog("power off: panel asleep\n");
    delay_ms(120);                                        /* (MIPI: sleep-in takes up to 120 ms) */
    uic_lcd_power(0);
    klog("power off: LCD power off\n");
    delay_ms(20);
}

/* show framebuffer `addr`: the address is set, then the next vertical blank waited for, where the panel takes it.
   Only then is the buffer shown until now free to draw into (waiting first and setting after left the old buffer on
   the screen for one more frame while the next picture was drawn into it: the keyboard flickered at each redraw) */
void lcd_show(u32 addr) { LCD(0x74) = addr; lcd_wait_vblank(); }

void lcd_wait_vblank(void) {
    u32 v = vblanks, t = time_us();
    while (vblanks == v && time_us() - t < 20000) __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));   /* sleep until an interrupt */
}
