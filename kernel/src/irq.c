/* Interrupts: the controller at 0xF0001200 as the stock firmware drives it (LVC 0x533e2 init, 0x53424 enable,
 * 0x53458 disable, 0x53482 acknowledge). Per interrupt n < 32: a config word at +0x08 + 4n (bits 0-3 priority,
 * bit 6 masked). Reading 0xF00013F0 acknowledges the highest-priority pending one (bit 7 set: none).
 * Known users: timers 0-7 = IRQ 0-7, SPI DMA 8-11, I2C 15, LCD vertical blank 22, audio 23 (the emulator). */
#include "os.h"

#define IC_CFG(n)  REG(0xf0001208u + 4u * (n))
#define IC_ACK     REG(0xf00013f0u)
#define IC_LEVEL   REG(0xf00013f8u)

static irq_fn handlers[32];
volatile u32 ticks_ms;

void irq_init(void) {
    for (int n = 0; n < 32; n++) { IC_CFG(n) = 0x4f; REG(0xf0001420u + 4u * n) = 1; }   /* all masked, lowest priority */
    REG(0xf00019d8u) = 1; REG(0xf00019dcu) = 0;           /* (stage 2 and LVC write these too; meaning unknown) */
    REG(0xf00013fcu) = 0; IC_LEVEL = 0xf;
}
/* before a restart: every source masked and cleared (stage 2 turns interrupts on; a source still armed would land in
   this kernel's handlers). Timer 1 keeps running: stage 2 counts its delays on it, as the boot ROM leaves it */
void irq_quiesce(void) {
    for (int n = 0; n < 32; n++) { IC_CFG(n) = 0x4f; REG(0xf0001420u + 4u * n) = 1; }
    for (int i = 0; i < 32 && !(IC_ACK & 0x80); i++) IC_LEVEL = 0xf;
}
void irq_attach(int n, int prio, irq_fn f) {
    handlers[n] = f;
    IC_CFG(n) = (IC_CFG(n) & ~0x4fu) | (u32)(prio & 0xf);
}
void irq_mask(int n) { IC_CFG(n) |= 0x40; }
void irq_dispatch(void) {
    for (;;) {
        u32 n = IC_ACK & 0xff;
        if (n & 0x80) break;
        if (n < 32 && handlers[n]) handlers[n]((int)n);
        IC_LEVEL = 0xf;   /* inferred: the stock driver writes the level back after a handler (LVC 0x534d8) */
    }
}
__attribute__((target("arm"))) void irq_on(void) { __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0", "memory"); }
__attribute__((target("arm"))) void irq_off(void) { __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0", "memory"); }

/* timer 1 (0xF0000420: +0 control, +4 count, +8 compare) at 1 kHz: control 0x22 = running, prescaler 2 (1 MHz / 8),
   compare 124: what stage 2 and LVC (0x5355e) program it to */
static void tick(int n) { (void)n; ticks_ms++; uart_rx_poll(); audio_tick(); sched_tick(); }
void timer_init(void) {
    REG(0xf0000420u) = 0x20;
    REG(0xf0000428u) = 124;
    REG(0xf0000420u) = 0x22;
    irq_attach(1, 8, tick);
}
