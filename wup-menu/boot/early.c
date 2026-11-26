/* The early hook: the very first thing Nintendo's firmware runs (its start-up at LVC 0x40, right after the vectors;
 * stage 2 enters at 0, which branches there). One ARM branch at 0x40 comes here; this code lives in 764 bytes of
 * unreferenced NFC debug strings at LVC 0x50408 (tools/mkflash.py checks them before writing).
 *
 * Nothing of Nintendo's firmware has run yet: stage 2 has set the clocks and done the UIC handshake, the MMU and the
 * caches are off. If A is held (the UIC's input report, read here over SPI) or the menu's Autoboot is on (its settings,
 * flash 0xB0000/0xB1000), the kernel is read from its
 * slot (flash 0x90000: header {"GPST", size, crc32}, the image from 0x91000, linked at RAM 0x10000), checked, and
 * started with r0 = "HOOK" (the flash then in 4-byte address mode): every chip as cold as after power-on, the start its drivers are proven from. In every
 * other case (no X, no valid kernel, anything unexpected) this returns, the displaced instruction runs, and Nintendo's
 * firmware starts exactly as it would have (stock; A at power-on: the menu, through the later hook, boot/hook.S).
 *
 * The SPI controller and its DMA as the kernel's spi.c drives them (proven on the pad): with the caches off, DMA goes
 * straight to and from RAM. */
typedef unsigned int u32; typedef unsigned char u8;
#define REG(a)   (*(volatile u32 *)(a))
#define UIC_CS   0xf00050fcu
#define FL_CS    0xf00050f8u
#define SYS_HDR  0x00090000u
#define SYS_IMG  0x00091000u
#define SYS_END  0x000b0000u
#define SYS_RAM  0x00010000u
#define KMENU_HDR 0x000b2000u                       /* the kernel's menu (kernel/src/app.c) */
#define SYS_MAGIC 0x54535047u                       /* "GPST" */
#define HOOK_ARG 0x4b4f4f48u                        /* "HOOK" */
#define BUF      0x00341000u                        /* (where the loader runs later: free memory to Nintendo's firmware,
                                                       which has not started yet; 4 KB, its stack above, to 0x343000) */
#define BTN_A    0x8000u
#define CFG0     0x000b0000u                        /* the menu's settings, two records (wup-menu/menu/config.c) */
#define CFG1     0x000b1000u

int early_main(void);
static void boot_kernel(u32 a_held);
#define FAR __attribute__((noinline, section(".text.far")))

/* the entry (ARM): a stack of our own, the check, then Nintendo's first instruction and on to its second */
__attribute__((naked, target("arm"), section(".text.entry"))) void early_entry(void) {
    __asm__ volatile(
        "ldr   r0, =0x00343000\n"                  /* everything stage 2 left in the registers kept, sp too */
        "stmdb r0!, {r1-r12, lr}\n"
        "str   sp, [r0, #-4]!\n"
        "mov   sp, r0\n"
        "blx   early_main\n"
        "ldr   sp, [sp]\n"
        "ldr   r0, =0x00343000 - 14 * 4\n"
        "ldmia r0, {r1-r12, lr}\n"
        "mov   r0, #0xdf\n"                         /* (LVC 0x40: mov r0, #0xdf, displaced by the branch here) */
        "ldr   pc, =0x00000044\n"
        ".ltorg\n");
}

__attribute__((noinline, target("arm"))) static void dcache_flush(void) {                    /* (whether or not stage 2 left the D-cache on: cleaned, then dropped) */
    __asm__ volatile("1: mrc p15, 0, apsr_nzcv, c7, c14, 3\n bne 1b\n mcr p15, 0, %0, c7, c6, 0\n mcr p15, 0, %0, c7, c10, 4"
                     :: "r"(0) : "memory", "cc");
}
static int until(volatile u32 *r, u32 bit, u32 want) { /* 0 once (*r & bit) == want, -1 if it never is */
    for (u32 t = 0; t < 4000000; t++) if ((*r & bit) == want) return 0;
    return -1;
}
static int dma(volatile u32 *d, u32 mode, u32 addr, u32 n) {   /* d: DMA channel 0 */
    dcache_flush();
    d[0] = 2; d[1] = mode; d[2] = 0; d[3] = 0; d[4] = n - 1; d[5] = addr; d[0] = 1;
    int e = until(d, 1, 0);
    d[0] = 2;
    dcache_flush();
    return e;
}
/* one transaction: cs down, nout bytes from out, then nin bytes to in, cs up */
static int xfer(u32 cs, const u8 *out, u32 nout, u32 in, u32 nin) {
    volatile u32 *sp = (volatile u32 *)0xf0004400u, *d = (volatile u32 *)0xf0004040u;
    d[-16] |= 0x8003;                                /* (0xF0004000: DMA on) */
    sp[0] = 0x8018; REG(cs) = 0xc200;
    sp[1] &= ~0x202u;
    sp[6] |= 0x80;
    int e = dma(d, 5, (u32)out, nout);
    if (!e) e = until(sp + 2, 0x80, 0x80);
    sp[2] = 0x80; sp[6] &= ~0x80u;
    if (!e && nin) {
        sp[1] |= 0x40; for (volatile u32 w = 0; w < 4000; w++) {} sp[1] &= ~0x40u;
        sp[1] |= 0x2;
        sp[8] = nin;                                 /* (at most 4 KB at a time: flash() asks so) */
        e = dma(d, 4, in, nin);
        sp[8] = 0;
    }
    sp[1] |= 0x200; REG(cs) = 0xc300;
    return e;
}
FAR static int flash(u32 addr, u32 to, u32 n) {         /* (4-byte addresses: early_main sets the flash so, as the kernel does) */
    u8 c[8] __attribute__((aligned(4)));
    while (n) {
        u32 k = n < 4096 ? n : 4096;
        c[0] = 0x03; c[1] = (u8)(addr >> 24); c[2] = (u8)(addr >> 16); c[3] = (u8)(addr >> 8); c[4] = (u8)addr;
        if (xfer(FL_CS, c, 5, to, k)) return -1;
        addr += k; to += k; n -= k;
    }
    return 0;
}
FAR static u32 crc32(u32 c, const u8 *p, u32 n) {   /* (c: ~0 to start, the running value after; ~ at the end) */
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1; }
    return c;
}

__attribute__((noinline, noreturn, naked, target("arm"), section(".text.far"))) static void start_kernel(void) {   /* I-cache dropped, then in */
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0\n ldr r0, =0x4b4f4f48\n ldr r1, =0x00010000\n bx r1\n .ltorg");   /* "HOOK", SYS_RAM */
}

/* the SPI controller and DMA as stage 2 left them, given back on the way to Nintendo's start (its drivers take them
   as they find them) */
static u32 saved[10];
static const u32 regs[10] = { 0xf0004000u, 0xf0004400u, 0xf0004404u, 0xf0004418u, 0xf0004420u,
                              0xf0004040u, 0xf0004044u, 0xf0004050u, 0xf0004054u, UIC_CS };
static void save(void) { for (int i = 0; i < 10; i++) saved[i] = REG(regs[i]); }
static void restore(void) { for (int i = 9; i >= 0; i--) if (i != 5) REG(regs[i]) = saved[i]; }

int early_main(void) {
    save();
    const u8 *r = (const u8 *)BUF;
    static const u8 report = 0x07;
    u32 held = 0;
    for (int i = 0; i < 20; i++) {                  /* (the first answers can be all zeros: asked again) */
        if (!xfer(UIC_CS, &report, 1, BUF, 128) && r[127] == (u8)~r[0]) { held = (u32)(r[2] << 8 | r[3]); break; }
        for (volatile u32 w = 0; w < 200000; w++) ;
    }
    boot_kernel(held & BTN_A);                                 /* (back only if there is no whole kernel) */
    restore();
    return 0;
}

/* the X path, in the second area (FAR) */
static const u8 fcmd[4] = { 0x06, 0xb7, 0x06, 0xe9 };  /* write enable + enter / exit 4-byte addresses */
FAR static void addr4(int on) { xfer(FL_CS, &fcmd[on ? 0 : 2], 1, 0, 0); xfer(FL_CS, &fcmd[on ? 1 : 3], 1, 0, 0); }
/* 1: the kernel whole, checked through BUF first (nothing of Nintendo's in RAM touched), then copied to SYS_RAM */
FAR static int kernel_whole(void) {
    const u32 *h = (const u32 *)BUF;
    if (flash(KMENU_HDR, BUF, 8) || h[0] != 0x554e4d57u || h[1] < 64) return 0;   /* its menu there ("WMNU"; written last,
                                                       after a write checked read back: whole), or stock */
    if (flash(SYS_HDR, BUF, 16) || h[0] != SYS_MAGIC || h[1] < 0x100 || h[1] > SYS_END - SYS_IMG) return 0;
    u32 size = h[1], want = h[2], c = ~0u;
    for (u32 o = 0; o < size; o += 4096) {
        u32 k = size - o < 4096 ? size - o : 4096;
        if (flash(SYS_IMG + o, BUF, k)) return 0;
        c = crc32(c, (const u8 *)BUF, k);
    }
    if (~c != want) return 0;
    if (flash(SYS_IMG, SYS_RAM, size)) for (;;) ;    /* (Nintendo's start already overwritten: power off and on.
                                                       A read that worked just before fails only if the pad is failing) */
    return 1;
}
/* the menu's Autoboot: the valid record with the higher sequence (config.c: {magic "WCFG", version 1, seq, len 76, crc32},
   then config_t: light, autoboot, ...) */
FAR static int autoboot(void) {
    u32 best = 0, seq = 0;
    for (u32 a = CFG0; a <= CFG1; a += CFG1 - CFG0) {
        const u32 *r = (const u32 *)BUF;
        if (flash(a, BUF, 96) || r[0] != 0x47464357u || r[1] != 1 || r[3] != 76 || ~crc32(~0u, (const u8 *)BUF + 20, 76) != r[4]) continue;
        if (!best || r[2] > seq) { seq = r[2]; best = 1 + ((const u8 *)BUF)[21]; }
    }
    return best > 1;
}
FAR static void boot_kernel(u32 a_held) {              /* (the kernel's flash start, kernel/src/flash.c: 4-byte addresses) */
    addr4(1);
    if ((a_held || autoboot()) && kernel_whole()) start_kernel();
    addr4(0);                                          /* (no whole kernel: the flash given back as Nintendo expects it) */
}
