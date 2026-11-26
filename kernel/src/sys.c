/* Restarting: Nintendo's stage 2 from flash 0 to RAM 0x3F0000 and into it with interrupts, the MMU and the caches off,
 * as the boot ROM starts it (ROM 0x224). Nintendo's firmware then starts, stock: the kernel leaves word for the hooks
 * not to start it again (Autoboot would). */
#include "os.h"

int sys_reboot(void) {
    u32 size = 0;
    static u8 code[0xe000];
    if (flash_read(0, &size, 4) || !size || size > 0xe000 - 0x44) return -1;
    if (flash_read(0x44, code, size)) return -2;
    u32 entry;                                            /* where the boot ROM goes in: the reset vector (Nintendo's:
                                                             0x3F0018; its first words are exception handlers) */
    static u32 vec[16];                                   /* its vector table (flash 4..0x43): at address 0, as the boot ROM
                                                             puts it (stage 2 runs its code in user mode and calls SWIs) */
    if (flash_read(4, vec, sizeof vec)) return -3;
    entry = vec[8];                                       /* (the reset vector's target: ldr pc, [pc, #0x18]) */
    if (vec[0] != 0xe59ff018u || entry < 0x3f0000u || entry >= 0x3f0000u + size) return -4;
    klog("restart\n");
    irq_off();
    irq_quiesce();
    REG(0xf0005420u) |= 1;                                /* (sound muted) */
    memcpy((void *)0x3f0000, code, size);
    for (int i = 0; i < 16; i++) ((volatile u32 *)0)[i] = vec[i];   /* (volatile: a write to 0 is not optimised away) */
    flash_done();
    __asm__ volatile(
        "1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n bne 1b\n"
        "mcr p15, 0, %0, c7, c10, 4\n"
        "mrc p15, 0, r0, c1, c0, 0\n"
        "bic r0, r0, #0x1000\n bic r0, r0, #0x5\n"
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mcr p15, 0, %0, c7, c7, 0\n"
        "mcr p15, 0, %0, c8, c7, 0\n"
        "bx %1\n" :: "r"(0), "r"(entry) : "r0", "memory", "cc");
    for (;;) ;
}
