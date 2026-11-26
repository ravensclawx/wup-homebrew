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
    u16 *fb = (u16 *)REG(0xf0009474u);                   /* (whatever the screen shows: red) */
    if ((u32)fb >= 0x100000u && (u32)fb < RAM_SIZE) for (u32 i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0xa000;
    for (;;) ;
}
