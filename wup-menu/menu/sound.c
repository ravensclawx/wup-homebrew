/* sound: the menu's one sound, the click: when the selection moves, something opens, B goes back, or an alert or a
 * notice appears. 8 ms, made at start with integer maths (wup_tone.h). While a computer is connected to the debug
 * server (Developer Mode) the menu is silent: tools/install.py puts its helper code in the audio ring's memory. */
#include "menu.h"
#include "dbgsrv.h"

#include "wup_tone.h"

static int16_t click[2 * CLICK];
static int ready, open_;
static uint32_t last;

static int on(void) {
    if (dbgsrv_active()) return 0;
    if (!open_) { if (wup_audio_open()) { wup_log("[menu] sound: the audio did not start\n"); return 0; } open_ = 1; }
    return 1;
}
void sound_init(void) {
    make_click(click);
    ready = 1;
}
void sound_off(void) {
    if (open_) { wup_audio_close(); open_ = 0; }
}
void sound_click(void) {
    if (!ready || M.cfg.sound_off) return;
    uint32_t now = wup_us();
    if (now - last < 40000) return;                 /* one click for one action (a press that also moves the focus) */
    last = now;
    if (on()) wup_audio_queue(click, CLICK);
}
void sound_note(int notice) { (void)notice; sound_click(); }   /* an alert or a notice appears (wup_alert_sound) */
