/* The board: the stock firmware's init replayed (fw_init.c), the chip revision, and time. */
#include "os.h"
#include "fw_init.h"

void fw_replay(const fw_write *w, int n) {
    for (int i = 0; i < n; i++, w++) {
        if (w->bits == 16) {
            u16 v = w->rmw ? (u16)((REG16(w->addr) & ~w->clr) | w->set) : (u16)w->set;
            REG16(w->addr) = v;
        } else {
            REG(w->addr) = w->rmw ? (REG(w->addr) & ~w->clr) | w->set : w->set;
        }
    }
}

/* 0xF0000000 bits 0-7: the stock board init (LVC 0x88a9a) sets the pins up one way for 0x41, another for the rest */
u32 soc_rev(void) { return REG(0xf0000000u) & 0xff; }

/* Pad config, pins, clocks, block resets, timer prescalers: what LVC 0x88a9a does as the stock firmware starts
   (called from its reset code, LVC 0x40 -> 0x884e0). After it the 0xF0000408 counter runs at ~1 MHz
   (108 MHz / (0x6b + 1)). */
void board_init(void) {
    if (soc_rev() == 0x41) fw_replay(fw_board_41, fw_board_41_n);
    else fw_replay(fw_board, fw_board_n);
}

/* the ~1 MHz counter: 9973/10000 MHz, measured against the console's clock (the emulator devices.c, LVC 0x1cd4e) */
u32 time_us(void) { return REG(0xf0000408u); }
void delay_us(u32 us) { u32 t = time_us(); while (time_us() - t < us) ; }
void delay_ms(u32 ms) { while (ms--) delay_us(1000); }
