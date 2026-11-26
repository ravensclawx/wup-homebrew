/* The input: the UIC's 128-byte report (buttons, sticks, touch, motion, battery flags), read every 4 ms by a task of
 * its own, as Nintendo's input task did; libwup reads input_report as it read Nintendo's copy (0x178D38). */
#include "os.h"

u8 input_report[128] __attribute__((aligned(4)));
volatile u32 input_seq;

static void input_task(void *unused) {
    (void)unused;
    for (;;) {
        if (!uic_poll()) { memcpy(input_report, uic_report, 128); input_seq++; }
        sched_sleep(4);
    }
}
void input_start(void) {
    if (!uic_poll()) { memcpy(input_report, uic_report, 128); input_seq++; }   /* (the buttons held at power-on, at once) */
    task_start(input_task, NULL, NULL, 4096, 4, "input");
}
