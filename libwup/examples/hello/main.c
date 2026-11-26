/* Hello world: the smallest libwup app. B goes back to the menu. */
#include "wup.h"

WUP_APP("Hello World");

int main(void) {
    wup_screen();                                        /* the 854x480 RGB565 screen */
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(24, 44, 96), C24(6, 10, 30));
    gfx_text_c(&font_big, GFX_W / 2, 220, "Hello, world!", RGB(255, 255, 255));
    gfx_text_c(&font_body, GFX_W / 2, 280, "Press B to return to the menu", RGB(190, 200, 225));
    gfx_present_all();

    while (wup_buttons() & WUP_B) wup_sleep(16);         /* not a B still held from before */
    while (!(wup_buttons() & WUP_B)) wup_sleep(16);      /* (wup_sleep gives the other tasks a turn) */
    return 0;                                            /* back to the menu */
}
