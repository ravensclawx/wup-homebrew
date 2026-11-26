/* video and the camera (wup.h): the kernel's decoder and camera drivers (wupk.h) */
#include <stdlib.h>
#include "wup.h"
#include "wupk.h"

int wup__video_on;                                   /* (gfx.c: nothing shown while it is) */
int wup_video_open(void) {
    if (wup__video_on) return 0;
    if (WUPK->video_open()) return -1;
    wup__video_on = 1;
    return 0;
}
int wup_video_frame(const void *d, unsigned n, int key) { return wup__video_on ? WUPK->video_frame(d, n, key) : -1; }
int wup_video_need_key(void) { return WUPK->video_need_key(); }
int wup_video_waiting(void) { return WUPK->video_waiting(); }
void wup_video_close(void) { if (wup__video_on) { WUPK->video_close(); wup__video_on = 0; } }
unsigned wup_us_since_vblank(void) { return WUPK->us_since_vblank(); }
const volatile uint8_t *wup_input_report(void) { return WUPK->report; }

static uint8_t *cam_mem;
int wup_camera_open(void) {
    if (cam_mem) return 0;
    if (wup__video_on) return -1;
    cam_mem = malloc(WUPK->cam_frame_size + 32);
    if (!cam_mem) return -1;
    if (WUPK->cam_open((uint8_t *)(((uintptr_t)cam_mem + 31) & ~(uintptr_t)31))) { free(cam_mem); cam_mem = 0; return -2; }
    return 0;
}
int wup_camera_draw(int x, int y, int mirror) {
    if (!cam_mem || wup__video_on || x < 0 || y < 0 || x > GFX_W - 640 || y > GFX_H - 480) return -1;
    if (!WUPK->cam_ready()) return 0;                /* (the last picture stays: presenting carries it over) */
    WUPK->cam_convert(gfx_fb() + y * GFX_W + x, GFX_W, mirror);
    WUPK->cam_next();
    return 1;
}
void wup_camera_close(void) { if (cam_mem) { WUPK->cam_close(); free(cam_mem); cam_mem = 0; } }

int wup_camera_encode_open(void) { return wup__video_on && !cam_mem ? WUPK->cam_enc_open() : -1; }
int wup_camera_encode_read(void *f, int max) { return WUPK->cam_enc_read((uint8_t *)f, (uint32_t)max); }
void wup_camera_encode_restart(void) { WUPK->cam_enc_restart(); }
unsigned wup_camera_encode_starts(void) { return WUPK->cam_enc_starts(); }
void wup_camera_encode_close(void) { WUPK->cam_enc_close(); }
