/* sys: small firmware services for the menu (LVC rev 7677).
 * UIC queries: the UIC (the pad's microcontroller) answers one-byte commands; the firmware's driver
 * keeps a state block whose address is at 0x13434 (byte 0 = 1 when ready, +4 = driver context; the
 * context's +0x10 points at the command buffer) and serialises access with semaphore 0x1b
 * (wai_sem 0x8b64c / sig_sem 0x8ae1e, thumb). Transfer: 0x115f4(ctx, tx_len, rx_len, rx_buf),
 * as the firmware's own UIC 0x13 query does (LVC 0x131d2). UIC 0x10 = battery level 0..6. */
#include "sys.h"
typedef unsigned u32; typedef unsigned char u8;
#ifdef WUP_KERNEL                                    /* (the wup-homebrew kernel: include/wupk.h) */
#include "wupk.h"
#define FW_REPORT (WUPK->report)
int uic_query(int cmd) { u8 c = (u8)cmd, v = 0; return WUPK->uic(&c, 1, &v, 1) ? -1 : v; }
#else
typedef int (*sem_fn)(int id, int tmo);
typedef int (*sig_fn)(int id);
typedef int (*xfer_fn)(u32 ctx, int ntx, int nrx, u8 *rx);
#define WAI_SEM ((sem_fn)0x0008b64d)
#define SIG_SEM ((sig_fn)0x0008ae1f)
#define UIC_XFER ((xfer_fn)0x000115f5)
#define FW_REPORT ((const volatile u8 *)0x00178d38u)

int uic_query(int cmd) {
    u32 st = *(volatile u32 *)0x00013434u;
    if (!st || *(volatile u8 *)st != 1) return -1;
    u32 ctx = *(volatile u32 *)(st + 4);
    if (!ctx) return -2;
    if (WAI_SEM(0x1b, -1)) return -3;
    u8 *buf = *(u8 **)(ctx + 0x10);
    buf[0] = (u8)cmd;
    UIC_XFER(ctx, 1, 1, buf + 1);
    int v = buf[1];
    SIG_SEM(0x1b);
    return v;
}
#endif
void sys_power(power_t *p) {
    u8 s = FW_REPORT[4];
    p->ac = s & 1; p->charging = (s >> 6) & 1; p->headphones = (s >> 5) & 1;
    p->raw = FW_REPORT[5];
    int lv = uic_query(0x10);
    p->level = lv;
    p->percent = lv >= 0 && lv <= 6 ? (lv * 100 + 3) / 6 : -1;
    if (p->charging && lv == 0) p->percent = -1;         /* the UIC reports level 0 while charging (seen) */
}
