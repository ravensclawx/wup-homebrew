/* The SPI controller (0xF0004400) and its DMA, for the two chips on it: the UIC and the flash. Each has its chip
 * select on a pin, as LVC drives them (0xF00050FC the UIC, 0xF00050F8 the flash: 0xC200 selected, 0xC300 not).
 * The bytes move by DMA, the firmware's way (LVC 0x10fa8-0x11720), finished the way the UART flasher finishes a transfer on
 * the pad (an earlier UART flasher):
 *   controller +0x00 clock (0x8018: 8 MHz), +0x04 bit 1 receive / bit 6 flush / bit 9 idle, +0x08 status (bit 7
 *   written, write 1 to clear), +0x18 its enable, +0x20 bytes to receive.
 *   DMA channel 0 (0xF0004040): +0x04 mode (5 RAM to SPI, 4 SPI to RAM), +0x10 count - 1, +0x14 RAM address,
 *   +0x00 1 start / bit 0 busy / 2 stop. 0xF0004000 |= 0x8003 turns DMA on (LVC 0x79a4).
 * By hand through the 16-byte FIFO, long replies overran on the pad (2026-11-19). */
#include "os.h"

#define SPI(o)  REG(0xf0004400u + (o))
#define DMA(o)  REG(0xf0004040u + (o))
#define TIMEOUT 20000
#define CHUNK   4096

static u8 buf[CHUNK] __attribute__((aligned(32)));      /* the DMA's side: whole cache lines */

static void clean(u32 n) {
    for (u32 a = (u32)buf; a < (u32)buf + n; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    dsb();
}
static void invalidate(u32 n) {
    for (u32 a = (u32)buf; a < (u32)buf + n; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
}

static int dma(u32 mode, u32 n) {
    u32 t;
    for (t = time_us(); DMA(0x00) & 1; ) if (time_us() - t > TIMEOUT) break;
    DMA(0x00) = 2;
    DMA(0x04) = mode;
    DMA(0x08) = 0; DMA(0x0c) = 0;
    DMA(0x10) = n - 1;
    DMA(0x14) = (u32)buf;
    DMA(0x00) = 1;
    int err = 0;
    for (t = time_us(); DMA(0x00) & 1; ) if (time_us() - t > TIMEOUT) { err = -1; break; }
    DMA(0x00) = 2;
    return err;
}

static int send(const u8 *p, u32 n) {
    while (n) {
        u32 k = n < CHUNK ? n : CHUNK;
        memcpy(buf, p, k);
        clean(k);
        SPI(0x18) |= 0x80;
        int err = dma(5, k);
        for (u32 t = time_us(); !err && !(SPI(0x08) & 0x80); ) if (time_us() - t > TIMEOUT) err = -1;
        SPI(0x08) = 0x80;
        SPI(0x18) &= ~0x80u;
        if (err) return err;
        p += k; n -= k;
    }
    return 0;
}

static int receive(u8 *p, u32 n) {
    SPI(0x04) |= 0x40; delay_us(60); SPI(0x04) &= ~0x40u;   /* flush what came in while sending (the UART flasher: 60 us) */
    SPI(0x04) |= 0x2;                                       /* receive */
    while (n) {
        u32 k = n < CHUNK ? n : CHUNK;
        SPI(0x20) = k;
        clean(k);
        int err = dma(4, k);
        invalidate(k);
        if (err) { SPI(0x20) = 0; return -2; }
        memcpy(p, buf, k);
        p += k; n -= k;
    }
    SPI(0x20) = 0;
    return 0;
}

/* one transaction: chip select `cs` (a pin register) down, out[], then in[] (or a second block to send: out2[]),
   chip select up */
static int xfer(u32 cs, const u8 *out, u32 nout, const u8 *out2, u32 nout2, u8 *in, u32 nin) {
    REG(0xf0004000u) |= 0x8003;
    SPI(0x00) = 0x8018;
    REG(cs) = 0xc200;
    SPI(0x04) &= ~0x200u;
    SPI(0x04) &= ~0x2u;                                     /* transmit */
    int err = send(out, nout);
    if (!err && nout2) err = send(out2, nout2);
    if (!err && nin) err = receive(in, nin);
    SPI(0x04) |= 0x200;
    REG(cs) = 0xc300;
    return err;
}

/* one task on the bus at a time (the flash and the UIC share it; kapi.c's lock) */
int spi_xfer(u32 cs, const u8 *out, u32 nout, const u8 *out2, u32 nout2, u8 *in, u32 nin) {
    bus_lock();
    int r = xfer(cs, out, nout, out2, nout2, in, nin);
    bus_unlock();
    return r;
}
