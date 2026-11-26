/* Restarting: Nintendo's stage 2 from flash 0 to RAM 0x3F0000 and into it with interrupts, the MMU and the caches off,
 * as the boot ROM starts it (ROM 0x224). Nintendo's firmware then starts; the hook and the loader pick by the buttons
 * held, as at power-on. */
#include "os.h"

int sys_reboot(void) {
    u32 size = 0;
    static u8 code[0xe000];
    if (flash_read(0, &size, 4) || !size || size > 0xe000 - 0x44) return -1;
    if (flash_read(0x44, code, size)) return -2;
    klog("restart\n");
    irq_off();
    REG(0xf0005420u) |= 1;                                /* (sound muted) */
    memcpy((void *)0x3f0000, code, size);
    flash_done();
    __asm__ volatile(
        "1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n bne 1b\n"
        "mcr p15, 0, %0, c7, c10, 4\n"
        "mrc p15, 0, r0, c1, c0, 0\n"
        "bic r0, r0, #0x1000\n bic r0, r0, #0x5\n"
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mcr p15, 0, %0, c7, c7, 0\n"
        "mcr p15, 0, %0, c8, c7, 0\n"
        "bx %1\n" :: "r"(0), "r"(0x3f0000u) : "r0", "memory", "cc");
    for (;;) ;
}
