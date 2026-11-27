/* The input: the UIC's 128-byte report (buttons, sticks, touch, motion, battery flags), read every 4 ms by a task of
 * its own, as Nintendo's input task did; libwup reads input_report as it read Nintendo's copy (0x178D38). */
#include "os.h"

u8 input_report[128] __attribute__((aligned(4)));
volatile u32 input_seq;

/* The Power button (report byte 4, bit 1), held a second: the pad off, the screen first, as the stock firmware does. Only
   after it has been seen up: it may still be held from switching the pad on */
static void input_task(void *unused) {
    (void)unused;
    int armed = 0, held = 0;
    for (;;) {
        if (!uic_poll()) {
            memcpy(input_report, uic_report, 128); input_seq++;
            audio_volume(input_report[14]);                  /* the volume slider, in every app, as the stock firmware
                                                               does (the codec is written only when it moves) */
            int down = (input_report[4] & 0x02) != 0;
            if (!down) { armed = 1; held = 0; }
            else if (armed && ++held == 250) power_off();     /* (held 1 s, as stock: measured in wup-emu, which
                                                                 ignores 0.9 s and turns off at 1 s, still held) */
        }
        sched_sleep(4);
    }
}
void input_start(void) {
    if (!uic_poll()) { memcpy(input_report, uic_report, 128); input_seq++; }   /* (the buttons held at power-on, at once) */
    task_start(input_task, NULL, NULL, 4096, 4, "input");
}
