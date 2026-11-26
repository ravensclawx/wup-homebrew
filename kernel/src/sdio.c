/* SDIO through the SD host controller at 0xE0010000 (a standard SDHCI: the stock firmware's register use and
 * the emulator's model, src/wifi.c), for the Wi-Fi radio. Data moves through the buffer data port (+0x20) by hand, as the
 * stock firmware moves it (no DMA).
 *
 *   +0x04 block size (16)  +0x06 block count (16)  +0x08 argument  +0x0C transfer mode (16)  +0x0E command (16)
 *   +0x10 response  +0x20 data port  +0x24 present state  +0x28 host control (8)  +0x29 power (8)
 *   +0x2C clock control (16)  +0x2E timeout (8)  +0x2F software reset (8)
 *   +0x30 normal interrupt status (16, write 1 to clear: bit 0 command done, 1 transfer done, 4 buffer write ready,
 *   5 buffer read ready, 8 card interrupt, 15 error)  +0x32 error status  +0x34/+0x36 status enables
 *   +0x38/+0x3A signal enables  +0x40 capabilities */
#include "os.h"

#define HC(o)    REG(0xe0010000u + (o))
#define HC16(o)  REG16(0xe0010000u + (o))
#define HC8(o)   (*(volatile u8 *)(0xe0010000u + (o)))

#define R_NONE 0x00
#define R_136  0x09                              /* (136 bits, CRC checked) */
#define R_48   0x1a                              /* 48 bits, CRC and index checked */
#define R_48NC 0x02                              /* 48 bits, no checks (R4) */
#define R_48B  0x1b                              /* 48 bits with busy */
#define DATA   0x20

/* the controller's state, for error messages */
const char *sdio_state(void) {
    static char b[64];
    snprintf(b, sizeof b, "present %08x, status %04x/%04x", HC(0x24), HC16(0x30), HC16(0x32));
    return b;
}

static int wait_int(u16 bits, u32 us) {
    for (u32 t = time_us(); ; ) {
        u16 s = HC16(0x30);
        if (s & 0x8000) {
            u16 e = HC16(0x32);
            HC16(0x32) = e; HC16(0x30) = s & 0x7eff;
            HC8(0x2f) = 6;                                /* (the CMD and DAT lines reset after an error) */
            for (u32 r = time_us(); HC8(0x2f) & 6 && time_us() - r < 10000; ) ;
            return -(int)e;
        }
        if ((s & bits) == bits) { HC16(0x30) = bits; return 0; }
        if (time_us() - t > us) return -0x10000;
    }
}

/* one command; *resp gets the first response word */
int sdio_cmd(int idx, u32 arg, int flags, u32 *resp) {
    for (u32 t = time_us(); HC(0x24) & 3; ) if (time_us() - t > 100000) return -0x20000;   /* CMD / DAT busy */
    HC(0x08) = arg;
    HC16(0x0e) = (u16)(idx << 8 | flags);
    int e = wait_int(1, 100000);
    if (!e && resp) *resp = HC(0x10);
    return e;
}

/* CMD52: one byte of function `fn` at `addr`; a write returns what the card read back */
int sdio_rw8(int write, int fn, u32 addr, u8 v, u8 *out) {
    u32 r = 0;
    int e = sdio_cmd(52, (u32)write << 31 | (u32)fn << 28 | (write && out ? 1u << 27 : 0) | (addr & 0x1ffff) << 9 | v, R_48, &r);
    if (e) return e;
    if (r & 0xcb00) return -(int)(r & 0xcb00) - 0x40000;   /* R5 flags: COM_CRC, ILLEGAL, ERROR, FUNCTION, RANGE */
    if (out) *out = (u8)r;
    return 0;
}
int sdio_read8(int fn, u32 addr, u8 *v) { return sdio_rw8(0, fn, addr, 0, v); }
int sdio_write8(int fn, u32 addr, u8 v) { return sdio_rw8(1, fn, addr, v, NULL); }

/* CMD53: n bytes to or from function `fn` at `addr` (incrementing when `inc`), in blocks of `blksz` when n is a
   multiple of it and more than one block, otherwise in byte mode (n <= 512). p 4-byte aligned. */
int sdio_rw(int write, int fn, u32 addr, int inc, void *p, u32 n, u32 blksz) {
    int blocks = n > blksz && n % blksz == 0;
    u32 count = blocks ? n / blksz : n, size = blocks ? blksz : n;
    if (!blocks && n > 512) return -0x50000;
    HC16(0x04) = (u16)(0x7000 | size);                   /* (the stock driver sets the boundary bits too) */
    HC16(0x06) = (u16)(blocks ? count : 1);
    HC16(0x0c) = (u16)((write ? 0 : 0x10) | (blocks ? 0x22 : 0));      /* read, multi-block + block count */
    u32 arg = (u32)write << 31 | (u32)fn << 28 | (blocks ? 1u << 27 : 0) | (inc ? 1u << 26 : 0) | (addr & 0x1ffff) << 9 |
              (count & 0x1ff);
    int e = sdio_cmd(53, arg, R_48 | DATA, NULL);
    u32 *w = p;
    for (u32 b = 0; !e && b < (blocks ? count : 1); b++) {
        if ((e = wait_int(write ? 0x10 : 0x20, 200000))) break;
        for (u32 i = 0; i < (size + 3) / 4; i++) { if (write) HC(0x20) = *w++; else *w++ = HC(0x20); }
    }
    if (!e) e = wait_int(2, 200000);
    if (e) sdio_abort(fn);
    return e;
}

/* after a failed transfer: the card told to abort it (CCCR 0x06 = the function), the controller's data and command
   lines reset, the interrupt bits cleared, so the next command starts clean */
void sdio_abort(int fn) {
    HC8(0x2f) = 6;
    for (u32 t = time_us(); HC8(0x2f) & 6 && time_us() - t < 10000; ) ;
    HC16(0x30) = 0x7eff; HC16(0x32) = 0xffff;
    sdio_write8(0, 0x06, (u8)fn);
    HC8(0x2f) = 6;
    for (u32 t = time_us(); HC8(0x2f) & 6 && time_us() - t < 10000; ) ;
    HC16(0x30) = 0x7eff; HC16(0x32) = 0xffff;
}

/* the SD clock: base / div (div 1: the base clock), as the stock firmware's routine does it (LVC 0x3dd14) */
static int set_clock(u32 div) {
    HC16(0x2c) &= ~4u;
    HC16(0x2c) = (u16)((div >= 2 ? div / 2 : 0) << 8);
    HC16(0x2c) |= 1;
    for (u32 t = time_us(); !(HC16(0x2c) & 2); ) if (time_us() - t > 20000) return -1;
    HC16(0x2c) |= 4;
    HC16(0x36) = 0xffef; HC8(0x2e) = 0x0e; HC16(0x36) = 0xffff;    /* (the longest data timeout) */
    delay_ms(1);
    return 0;
}

/* The host and the card, as the stock firmware brings them up (wifi_spec: LVC's SDHCI and SDIO probe): reset, 390 kHz,
   3.3 V, 25 ms; CMD5 (until the card is ready), CMD3, CMD7; F1 on; 4-bit; F1 / F2 blocks of 64; interrupts on in the
   card; high speed; the base clock. Interrupts are polled. */
int sdio_init(void) {
    u32 caps = HC(0x40), r = 0;
    HC(0x00) = 0; HC8(0x28) = 0; HC16(0x38) = 0;
    HC8(0x2f) = 1;
    for (u32 t = time_us(); HC8(0x2f) & 1; ) if (time_us() - t > 100000) return -1;
    HC(0x00) = 0; HC8(0x28) = 0;
    HC16(0x30) = 0x01ff; HC16(0x32) = 0x0fff; HC16(0x34) = 0x01ff; HC16(0x36) = 0xffff; HC16(0x38) = 0;
    if (set_clock(128)) return -2;
    u8 volt = caps & (1u << 24) ? 7 : caps & (1u << 25) ? 6 : 5;
    HC8(0x29) = (u8)(volt << 1 | 1);                      /* bus power on, then CMD5: the stock start, nothing more.
                                                             (No I/O reset: a radio Nintendo's standby left with its
                                                             pads isolated is wedged by one; wifi.c un-isolates it as
                                                             the stock driver does.) */
    delay_ms(2);
    if (sdio_cmd(5, 0, R_48NC, &r)) {                     /* no answer: a card left mid-transfer by an earlier start
                                                             of ours (a restart from RAM): aborted, reset, idled */
        sdio_write8(0, 0x06, 0x01); sdio_write8(0, 0x06, 0x02);
        sdio_write8(0, 0x06, 0x08);
        delay_ms(100);
        sdio_cmd(0, 0, R_NONE, NULL);
        delay_ms(2);
        if (sdio_cmd(5, 0, R_48NC, &r)) return -3;
    }
    for (int i = 0; ; i++) {                              /* (the emulator is ready at once; a card may not be) */
        if (sdio_cmd(5, 0x00fff000, R_48NC, &r)) return -4;
        if (r & 0x80000000u) break;
        if (i == 100) return -5;
        delay_ms(10);
    }
    if (sdio_cmd(3, 0, R_48NC, &r)) return -6;
    u32 rca = r >> 16;
    if (sdio_cmd(7, rca << 16, R_48, &r)) return -7;
    int e = sdio_write8(0, 0x02, 0x02);
    u8 rdy = 0;
    for (u32 t = time_us(); !(rdy & 2); ) {               /* function 1 ready (after a reset it takes a moment) */
        if (sdio_read8(0, 0x03, &rdy)) rdy = 0;
        if (time_us() - t > 1000000) { klog("sdio: function 1 never ready (%02x)\n", rdy); return -10; }
    }
    e |= sdio_write8(0, 0x07, 0x02); HC8(0x28) = 0x02;     /* 4-bit */
    e |= sdio_write8(0, 0x110, 0x40) | sdio_write8(0, 0x111, 0) | sdio_write8(0, 0x210, 0x40) | sdio_write8(0, 0x211, 0);
    e |= sdio_write8(0, 0x04, 0x07);
    u8 hs = 0;
    e |= sdio_read8(0, 0x13, &hs);
    if (e) return -8;
    if (set_clock(2)) return -9;                          /* (24 MHz, no high speed: on the pad, data reads timed out
                                                             at 48 MHz high speed, 2026-11-19) */
    (void)hs;
    return 0;
}
