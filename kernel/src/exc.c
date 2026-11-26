/* A fault (undefined instruction, data or prefetch abort, SWI, FIQ): its registers to the log, the screen red, stop.
 * regs: r0-r12, then the faulting instruction's address. (An app's fault ends the pad's run: power off and on.) */
#include "os.h"

static const char *names[] = { "Undefined instruction", "SWI", "Prefetch abort", "Data abort", "FIQ" };

void fault(int kind, u32 *regs, u32 spsr) {
    u32 far, fsr;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0\n mrc p15, 0, %1, c5, c0, 0" : "=r"(far), "=r"(fsr));
    klog("\n%s at %p (cpsr %p, far %p, fsr %p, task %d)\n", names[kind], regs[13], spsr, far, fsr, sched_current());
    for (int i = 0; i < 13; i++) klog("r%d=%p%s", i, regs[i], i % 4 == 3 ? "\n" : " ");
    klog("\n");
    sched_dump();
    u16 *fb = (u16 *)FB0_ADDR;                            /* screen 0, RGB565, red, at full brightness (an app may
                                                             have left the 8-bit screen up, or the screen dark) */
    for (u32 i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0xa000;
    REG(0xf0009470u) = SCREEN_W; REG(0xf0009474u) = FB0_ADDR; REG(0xf00094b0u) = (REG(0xf00094b0u) & ~3u) | 3;
    for (u32 i = 0; i <= 32; i++) REG(0xf0009600u + 4 * i) = i * 8;
    for (;;) ;
}
