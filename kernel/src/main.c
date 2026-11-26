/* The wup-homebrew kernel: started by the loader (wup-menu boot/loader.c) with Nintendo's firmware stopped, the MMU
 * and caches off, r0 = HOOK_ARG. Nintendo's stage 2 set the clocks up; the rest of the pad is set up again here, the
 * way Wii U GamePad Streamer proved on the pad from the same start. Then tasks: input, the radio, the apps (the
 * Homebrew Menu first), and an idle one. */
#include "os.h"

void kapi_init(void);

static void idle(void *unused) {
    (void)unused;
    for (;;) __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));   /* sleep until an interrupt */
}

int main(u32 boot_arg) {
    uart_init();
    klog("\nwup-homebrew kernel: start, chip revision %02x%s\n", soc_rev(), boot_arg == HOOK_ARG ? "" : " (not from the loader)");
    irq_init();
    board_init();                                        /* (Nintendo's board init again: blocks reset to known states) */
    uart_init();
    mmu_init();
    u8 fid[3];
    int fe = flash_init(fid);
    timer_init();
    i2c_init();
    for (u32 i = 0; i < SCREEN_W * SCREEN_H; i++) ((u16 *)FB0_ADDR)[i] = 0;   /* (black until the menu draws) */
    irq_on();
    lcd_init(122, 1);
    audio_init();                                        /* (the codec and the sound block; libwup plays through it) */
    klog("kernel: flash %02x %02x %02x (%d), display and sound up, %u ms\n", fid[0], fid[1], fid[2], fe, ticks_ms);

    kapi_init();                                         /* (before any task: the bus lock, the table at WUPK_PTR) */
    input_start();
    devload_start();                                     /* (development: a new kernel over the UART, run from RAM) */
    task_start(idle, NULL, NULL, 1024, 255, "idle");
    task_start(apps_run, NULL, NULL, 32768, 10, "apps");
    sched_start();                                       /* (never returns) */
    return 0;
}
