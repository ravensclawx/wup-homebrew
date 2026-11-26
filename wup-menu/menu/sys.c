/* sys: the pad's power state for the menu: the input report's byte 4 (AC, charging, headphones) and the UIC's battery
 * level (command 0x10: 0..6), through the kernel. */
#include "sys.h"
typedef unsigned u32; typedef unsigned char u8;
#include "wupk.h"
#define FW_REPORT (WUPK->report)
int uic_query(int cmd) { u8 c = (u8)cmd, v = 0; return WUPK->uic(&c, 1, &v, 1) ? -1 : v; }
void sys_power(power_t *p) {
    u8 s = FW_REPORT[4];
    p->ac = s & 1; p->charging = (s >> 6) & 1; p->headphones = (s >> 5) & 1;
    p->raw = FW_REPORT[5];
    int lv = uic_query(0x10);
    p->level = lv;
    p->percent = lv >= 0 && lv <= 6 ? (lv * 100 + 3) / 6 : -1;
    if (p->charging && lv == 0) p->percent = -1;         /* the UIC reports level 0 while charging (seen) */
}
