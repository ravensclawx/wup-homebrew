/* The input: the UIC's 128-byte report (buttons, sticks, touch, motion, battery flags), read every 4 ms by a task of
 * its own, as Nintendo's input task did; libwup reads input_report as it read Nintendo's copy (0x178D38). */
#include "os.h"

u8 input_report[128] __attribute__((aligned(4)));
volatile u32 input_seq;

/* The Power button (report byte 4, bit 1): the pad off, the screen first, as the stock firmware turns it off. Only
   after it has been seen up: it may still be held from switching the pad on */
static void input_task(void *unused) {
    (void)unused;
    int armed = 0, held = 0;
    for (;;) {
        if (!uic_poll()) {
            memcpy(input_report, uic_report, 128); input_seq++;
            int down = (input_report[4] & 0x02) != 0;
            if (!down) { armed = 1; held = 0; }
            else if (armed && ++held == 10) power_off();      /* (40 ms down: a press, not a glitch) */
        }
        sched_sleep(4);
    }
}
void input_start(void) {
    if (!uic_poll()) { memcpy(input_report, uic_report, 128); input_seq++; }   /* (the buttons held at power-on, at once) */
    task_start(input_task, NULL, NULL, 4096, 4, "input");
}
