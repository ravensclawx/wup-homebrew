/* sound: the menu's sounds, made here with integer maths (no samples stored, no maths library):
 *   the chime  when the pad starts into the menu: four soft bell notes rising (E, B, E, G#), about 1.5 s;
 *   the click  when the selection moves, something opens or B goes back: 8 ms, under 2 KB.
 * Each note is a sine (a two-term recurrence) with a quieter octave above it, a 5 ms rise and an exponential fall.
 * The chime is made as it plays, by a thread of its own, keeping about 0.2 s queued.
 * While a computer is connected to the debug server (Developer Mode) the menu is silent: tools/install.py
 * puts its helper code in the audio ring's memory. */
#include "menu.h"
#include "dbgsrv.h"

#include "wup_tone.h"

static int16_t click[2 * CLICK];
#define CHIME (48000 * 3 / 2)                       /* 1.5 s */
static voice_t chime[8];
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
static int chime_thread = -1;
static volatile int chime_stop;
static void feed(void *arg) {                       /* the chime, made as it plays: in a thread, so a busy menu (the radio
                                                       coming up) cannot starve it */
    static int16_t buf[2 * 1200];
    (void)arg;
    for (int t = 0; t < CHIME && !chime_stop; ) {
        int n = wup_audio_room(); if (n > 1200) n = 1200; if (n > CHIME - t) n = CHIME - t;
        if (n < 240 && n < CHIME - t) { wup_thread_sleep(10); continue; }   /* (the chime's last few samples: whatever room) */
        for (int i = 0; i < n; i++) {
            int16_t v = render(chime, 8, t + i);
            if (t + i > CHIME - 4800) v = (int16_t)(v * (CHIME - t - i) / 4800);      /* the last 0.1 s fades */
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        wup_audio_queue(buf, n);                    /* (n fits: it was the room) */
        t += n;
    }
}
void sound_chime(void) {
    static const struct { int64_t c, c2; int32_t s, s2; int ms; } note[4] = {
        { 2139492403, 2115578141, 2824, 5627, 0 },              /* E5  659 Hz (+ E6) */
        { 2129557746, 2076151463, 4225, 8375, 90 },             /* B5  988 Hz (+ B6) */
        { 2115578141, 2020809671, 5627, 11088, 180 },           /* E6 1319 Hz (+ E7) */
        { 2096910565, 1866615822, 7069, 16202, 290 },           /* G#6 1661 Hz (+ G#7, near enough) */
    };
    if (!ready || M.cfg.sound_off || chime_thread >= 0 || !on()) return;
    for (int i = 0; i < 4; i++) {
        tone(&chime[2 * i], note[i].c, note[i].s, 2600, note[i].ms * 48, 1073692115);          /* falls by 1/e in 0.45 s */
        tone(&chime[2 * i + 1], note[i].c2, note[i].s2, 700, note[i].ms * 48, 1073652349);     /* the octave: 0.25 s */
    }
    chime_stop = 0;
    chime_thread = wup_thread_start(feed, 0, 4096, 1);
}
static int chiming(void) {
    if (chime_thread < 0) return 0;
    if (wup_thread_join(chime_thread, 0)) return 1;  /* (still playing) */
    chime_thread = -1;
    return 0;
}
void sound_off(void) {
    if (chime_thread >= 0) { chime_stop = 1; if (wup_thread_join(chime_thread, 200)) wup_thread_kill(chime_thread); chime_thread = -1; }
    if (open_) { wup_audio_close(); open_ = 0; }
}
void sound_click(void) {
    if (!ready || M.cfg.sound_off || chiming()) return;   /* (not over the chime) */
    uint32_t now = wup_us();
    if (now - last < 40000) return;                 /* one click for one action (a press that also moves the focus) */
    last = now;
    if (on()) wup_audio_queue(click, CLICK);
}
void sound_note(int notice) {                       /* an alert or a notice appears (wup_alert_sound) */
    if (!ready || M.cfg.sound_off || !on()) return;
    play_two_notes(!notice, wup_audio_queue);
}
void sound_launch(void) {                           /* an app opens */
    if (!ready || M.cfg.sound_off || !on()) return;
    play_launch(wup_audio_queue);
}
