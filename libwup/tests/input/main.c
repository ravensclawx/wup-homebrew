/* Input Test: every input libwup reads, live on the screen: buttons (all 19), both sticks, touch, the motion
 * sensors, the volume slider, battery, charger, headphones. A held rumbles while it is held. Home goes back. */
#include <stdio.h>
#include "wup.h"

WUP_APP("Input Test");

static const struct { unsigned b; const char *name; } BTN[] = {
    { WUP_A, "A" }, { WUP_B, "B" }, { WUP_X, "X" }, { WUP_Y, "Y" }, { WUP_UP, "Up" }, { WUP_DOWN, "Down" },
    { WUP_LEFT, "Left" }, { WUP_RIGHT, "Right" }, { WUP_L, "L" }, { WUP_R, "R" }, { WUP_ZL, "ZL" }, { WUP_ZR, "ZR" },
    { WUP_L3, "L3" }, { WUP_R3, "R3" }, { WUP_PLUS, "+" }, { WUP_MINUS, "-" }, { WUP_HOME, "Home" }, { WUP_TV, "TV" },
    { WUP_SYNC, "Sync" } };
#define WHITE RGB(255, 255, 255)
#define DIM   RGB(120, 130, 160)
#define LIT   RGB(90, 200, 120)

static void stick(int cx, int cy, int which, const char *name) {
    gfx_fill(cx - 60, cy - 60, 120, 120, RGB(30, 40, 70));
    int x = wup_stick(which, 0) * 55 / WUP_STICK_MAX, y = wup_stick(which, 1) * 55 / WUP_STICK_MAX;
    gfx_fill(cx + x - 6, cy - y - 6, 12, 12, LIT);
    gfx_text_c(&font_body, cx, cy + 86, name, DIM);
}

int main(void) {
    wup_screen();
    int touched_x = -1, touched_y = -1;
    unsigned seen = 0;
    for (;;) {
        unsigned held = wup_buttons();
        seen |= wup_pressed();
        if (held & WUP_HOME) break;
        wup_rumble(!!(held & WUP_A));
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(12, 18, 40));
        gfx_text(&font_big, 30, 50, "Input Test", WHITE);
        gfx_text(&font_body, 560, 50, "Home: back to the menu", DIM);
        for (unsigned i = 0; i < sizeof BTN / sizeof BTN[0]; i++) {     /* lit while held; outlined once seen */
            int x = 30 + (int)(i % 10) * 80, y = 80 + (int)(i / 10) * 46;
            gfx_fill(x, y, 72, 36, held & BTN[i].b ? LIT : seen & BTN[i].b ? RGB(40, 70, 60) : RGB(30, 40, 70));
            gfx_text_c(&font_body, x + 36, y + 25, BTN[i].name, WHITE);
        }
        stick(110, 270, 0, "Left stick");
        stick(290, 270, 1, "Right stick");
        int tx, ty;
        if (wup_touch(&tx, &ty)) { touched_x = tx; touched_y = ty; }
        wup_motion_t m; wup_motion(&m);
        int charging = 0, pct = wup_battery(&charging);
        char l[96];
        int y = 200;
        snprintf(l, sizeof l, "Touch      %s %d, %d", wup_touch(&tx, &ty) ? "down" : "up  ", touched_x, touched_y); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Accel      %6d %6d %6d", m.accel[0], m.accel[1], m.accel[2]); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Gyro       %8ld %8ld %8ld", (long)m.gyro[0], (long)m.gyro[1], (long)m.gyro[2]); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Magnet     %6d %6d %6d", m.mag[0], m.mag[1], m.mag[2]); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Volume     %d", wup_volume()); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Battery    %d%%%s", pct, charging ? ", charging" : ""); gfx_text(&font_body, 420, y, l, WHITE); y += 30;
        snprintf(l, sizeof l, "Charger    %s    Headphones  %s", wup_power_ac() ? "yes" : "no", wup_headphones() ? "yes" : "no");
        gfx_text(&font_body, 420, y, l, WHITE);
        gfx_text(&font_body, 30, 440, "Hold A: rumble", DIM);
        if (touched_x >= 0) gfx_fill(touched_x - 4, touched_y - 4, 8, 8, RGB(255, 200, 80));
        gfx_present_all();
        wup_sleep(16);
    }
    wup_rumble(0);
    return 0;
}
