/* UART1 on the battery-cover header (README: Installing on a GamePad), 115200 8N1: the kernel log and the shell (shell.c).
 * The register setup is an earlier homebrew library's, which logged from the pad this way. Registers (+0xF0004C40, the UART flasher's
 * regs.h, the boot ROM's loader at ROM 0x3c4): +0x00 received byte, +0x04 byte to send, +0x1C status (bit 0 a byte
 * waiting, bit 1 overrun, bit 5 room to send). Every wait has a limit: nothing hangs without a cable.
 *
 * Receiving: the main loop takes ~34 ms a frame, ~400 bytes at 115200, more than the UART's FIFO holds; the 1 kHz
 * timer tick (irq.c) empties it into a 64 KB ring instead (at most ~12 bytes arrive per tick). In the emulator: UART_LOG=1
 * for the log, --uart-pty for both directions. */
#include "os.h"

#define U(o)  REG(0xf0004c40u + (o))
#define RXN   65536u

void uart_init(void) {
    REG(0xf00050d4u) = 0x8001; REG(0xf00050d8u) = 0x0801;                 /* pins: UART1 TX / RX */
    REG(0xf0000058u) |= 1u << 8; REG(0xf0000044u) = 8 - 1; REG(0xf0000058u) &= ~(1u << 8);   /* reset; 32 MHz / 8 */
    U(0x30) = 1;
    U(0x24) = 1; U(0x28) = 288; U(0x2c) = 625;                              /* 115200 baud */
    U(0x10) = 0; U(0x14) = 3; U(0x18) = 0; U(0x08) = 0;                    /* 8N1, no interrupts */
}
static void uputc(char c) {
    for (int i = 0; i < 100000 && !(U(0x1c) & 0x20); i++) ;
    U(0x04) = (u8)c;
}
void uart_write(const void *p, u32 n) { const u8 *s = p; while (n--) uputc((char)*s++); }
void klog(const char *f, ...) {
    char b[256];
    va_list ap; __builtin_va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    __builtin_va_end(ap);
    for (const char *s = b; *s; s++) { if (*s == '\n') uputc('\r'); uputc(*s); }
}

static u8 rx[RXN];
static volatile u32 rx_w;
static u32 rx_r;
volatile u32 uart_overruns, uart_lost;

void uart_rx_poll(void) {                        /* (the 1 kHz tick) */
    for (int i = 0; i < 64; i++) {
        u32 st = U(0x1c);
        if (!(st & 1)) break;
        if (st & 2) uart_overruns++;
        u8 b = (u8)U(0x00);
        if (rx_w - rx_r < RXN) { rx[rx_w % RXN] = b; rx_w++; } else uart_lost++;
    }
}
/* the next received byte, or -1 after `timeout_us` (0: don't wait) */
int uart_getc(u32 timeout_us) {
    for (u32 t = time_us(); rx_w == rx_r; ) if (time_us() - t >= timeout_us) return -1;
    int c = rx[rx_r % RXN];
    rx_r++;
    return c;
}
