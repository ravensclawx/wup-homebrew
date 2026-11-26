/* Development: a new kernel over the UART cable, run from RAM at once (nothing written to flash): the pad stays as it
 * is (the radio as Nintendo's firmware left it), so a change is tried in seconds. tools: kernel/tools/uartrun.py.
 *   the computer sends: "WKRN", u32 size, u32 crc32, the image (linked at 0x10000)
 *   the kernel answers "ok\n" (then restarts into it with r0 = HOOK_ARG) or "bad\n" */
#include "os.h"

#define BUF  0x002fa400u                         /* (free while nothing uses it: below the loader's old slot) */
#define TRAMP 0x003f0000u
extern char relaunch_code[], relaunch_end[];     /* start.S: copies the image to 0x10000 and jumps to it */

static int getb(u32 us) { return uart_getc(us); }
static int get32(u32 *v) {
    u32 x = 0;
    for (int i = 0; i < 4; i++) { int c = getb(1000000); if (c < 0) return -1; x |= (u32)c << (8 * i); }
    *v = x;
    return 0;
}
static void say(const char *s) { uart_write(s, strlen(s)); }

static void devload_task(void *unused) {
    (void)unused;
    static const char magic[4] = { 'W', 'K', 'R', 'N' };
    for (int k = 0;;) {
        int c = getb(0);
        if (c < 0) { sched_sleep(20); continue; }
        k = (u8)c == (u8)magic[k] ? k + 1 : (u8)c == 'W' ? 1 : 0;
        if (k < 4) continue;
        k = 0;
        u32 size, crc;
        if (get32(&size) || get32(&crc) || size < 0x100 || size > 0x80000) { say("bad\n"); continue; }
        u8 *b = (u8 *)BUF;
        u32 got = 0;
        for (; got < size; got++) { int d = getb(1000000); if (d < 0) break; b[got] = (u8)d; }
        if (got != size || crc32(b, size) != crc) { say("bad\n"); continue; }
        say("ok\n");
        klog("devload: %u bytes, restarting into them\n", size);
        delay_ms(20);
        irq_off();
        __asm__ volatile(
            "1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n bne 1b\n"
            "mcr p15, 0, %0, c7, c10, 4\n"
            "mrc p15, 0, r0, c1, c0, 0\n"
            "bic r0, r0, #0x1000\n bic r0, r0, #0x5\n"
            "mcr p15, 0, r0, c1, c0, 0\n"
            "mcr p15, 0, %0, c7, c7, 0\n"
            "mcr p15, 0, %0, c8, c7, 0\n" :: "r"(0) : "r0", "memory", "cc");
        memcpy((void *)TRAMP, relaunch_code, (u32)(relaunch_end - relaunch_code));
        ((void (*)(const void *, void *, u32, u32))TRAMP)(b, (void *)0x10000, (size + 3) / 4, HOOK_ARG);
    }
}
void devload_start(void) { task_start(devload_task, NULL, NULL, 4096, 20, "devload"); }
