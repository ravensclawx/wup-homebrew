/* The camera: an OmniVision OV7740 (I2C 0x21) feeding the video input block (VIN, 0xF0009000), which writes frames
 * into RAM through the codec's windows (0xF0000818...). 640x480, about 55 frames a second.
 *
 * The frame in RAM (found on the pad, 2026-11-24): the codec's 4:2:0 layout, units of 48 bytes in raster order,
 * 40 to a line pair, 240 line pairs. A unit is 16 pixels of two lines: 16 Y of line 2r, 16 Y of line 2r+1, then 8 Cr
 * and 8 Cb (one pair for each 2x2). Each unit's pixels are stored right to left.
 *
 * The sensor: the stock boot's set-up (recorded in wup-emu) left it sending raw Bayer at half height. What works is a
 * soft reset, then the Linux driver's VGA YUYV table (drivers/media/i2c/ov7740.c, ov7740_vga + ov7740_format_yuyv:
 * register values), then the clock prescaler (0x11) at 0: with 0x02 (Linux) or 0x01 (stock) the sensor finishes only
 * part of each frame before VIN starts the next (240 lines with 0x01).
 *
 * VIN: the stock driver's start (drvvcapt, LVC 0x14824 reset, 0x14d3e configure, 0x156fe start), without the H.264
 * encoder. VIN +0x00 bit 0 runs the capture, frame after frame; bit 1 reads 1 while a frame is being written; the frame
 * interrupt (+0x28 bit 0) comes every frame. One frame: at a frame interrupt clear bit 0, then the frame in flight
 * finishes and bit 1 drops (proven 2026-11-24): the buffer holds a whole frame and VIN waits. Never touch
 * 0xF0000800 +0x00 (the memory controller; the stock |= 0x01000010 there makes RAM lose bits on wup-os). */
#include "os.h"

#define VIN(o)  REG(0xf0009000u + (o))
#define ENG(o)  REG(0xf0008c00u + (o))
#define WIN(o)  REG(0xf0000800u + (o))
#define SENSOR  0x21
#define CLOCK   REG(0xf0000038u)                          /* the sensor's clock: 0x423 on, 0xFE off (LVC 0x60f6, 0x61b6) */

static int sensor_w(u8 reg, u8 v) { u8 b[2] = { reg, v }; return i2c_write(SENSOR, b, 2); }

static const u8 vga_yuyv[] = {                            /* (register, value): ov7740_format_yuyv, then ov7740_vga */
    0x12,0x00, 0x36,0x3f, 0x80,0x7f, 0x83,0x01,
    0x55,0x40, 0x11,0x02, 0xd5,0x10, 0x0c,0x12, 0x0d,0x34, 0x17,0x25, 0x18,0xa0, 0x19,0x03, 0x1a,0xf0, 0x1b,0x89,
    0x22,0x03, 0x29,0x18, 0x2b,0xf8, 0x2c,0x01, 0x31,0xa0, 0x32,0xf0, 0x33,0xc4, 0x34,0x00, 0x35,0x05, 0x04,0x60,
    0x27,0x80, 0x3d,0x0f, 0x3e,0x80, 0x3f,0x40, 0x40,0x7f, 0x41,0x6a, 0x42,0x29, 0x44,0x22, 0x45,0x41, 0x47,0x02,
    0x49,0x64, 0x4a,0xa1, 0x4b,0x40, 0x4c,0x1a, 0x4d,0x50, 0x4e,0x13, 0x64,0x00, 0x67,0x88, 0x68,0x1a, 0x14,0x28,
    0x24,0x3c, 0x25,0x30, 0x26,0x72, 0x50,0x97, 0x51,0x1f, 0x52,0x00, 0x53,0x00, 0x20,0x00, 0x21,0xcf, 0x50,0x4b,
    0x38,0x14, 0xe9,0x00, 0x56,0x55, 0x57,0xff, 0x58,0xff, 0x59,0xff, 0x5f,0x04, 0xec,0x00, 0x13,0xff, 0x81,0x3f,
    0x82,0x32, 0x38,0x11, 0x84,0x70, 0x85,0x00, 0x86,0x03, 0x87,0x01, 0x88,0x05, 0x89,0x30, 0x8d,0x30, 0x8f,0x85,
    0x93,0x30, 0x95,0x85, 0x99,0x30, 0x9b,0x85, 0x0f,0x1d, 0x0f,0x1f,
    0x11,0x00 };                                          /* (the prescaler: the whole frame in time, see above) */

static int sensor_on(void) {
    CLOCK = 0x423;
    delay_ms(20);
    if (sensor_w(0x12, 0x80)) return -1;                  /* soft reset */
    delay_ms(10);
    int bad = 0;
    for (u32 i = 0; i < sizeof vga_yuyv; i += 2) bad += sensor_w(vga_yuyv[i], vga_yuyv[i + 1]) != 0;
    u8 id = 0;
    i2c_read(SENSOR, 0x0a, &id, 1);
    if (bad || id != 0x77) { klog("cam: sensor %02x, %d writes failed\n", id, bad); return -1; }
    return 0;
}
static void sensor_off(void) { sensor_w(0x0e, 0xe8); delay_ms(1); CLOCK = 0xfe; }   /* asleep, clock off (LVC 0x61b6) */

static void wait_bit(u32 a, u32 bit, u32 ms) { for (u32 t = ticks_ms; !(REG(a) & bit) && ticks_ms - t < ms; ) ; }

static void vin_reset(void) {                             /* LVC 0x14824 */
    VIN(0x28) |= 0x8000; ENG(0x70) |= 0x4000; REG(0xf0008468u) |= 0xf0000;
    VIN(0x00) |= 0x80000000u; VIN(0x00) &= ~1u; VIN(0x00) &= ~0x80000000u;
    ENG(0x2c) |= 0x30000; ENG(0x00) |= 2;
    ENG(0x74) |= 4; wait_bit(0xf0008c74u, 4, 20); ENG(0x74) |= 0x40; ENG(0x74) &= ~0x44u;
    VIN(0x24) |= 1; wait_bit(0xf0009024u, 1, 20); VIN(0x24) &= ~1u;
    u32 save[17];
    for (int i = 0; i < 17; i++) save[i] = VIN(i * 4);
    REG(0xf0000058u) |= 1u << 20; REG(0xf0000058u) &= ~(1u << 20);
    for (int i = 0; i < 17; i++) VIN(i * 4) = save[i];
    ENG(0x14) = 0; REG(0xf0008860u) |= 3; REG(0xf0008860u) &= ~3u; REG(0xf0008460u) = 0;
}

static u8 *frame;                                         /* CAM_FRAME bytes, 32-aligned; 0: the camera is off */
static int state;                                         /* 0 the frame is the reader's, 1 capturing, 2 finishing */
u32 cam_frames;

static void cache_inval(void *p, u32 n) {
    for (u32 a = (u32)p & ~31u; a < (u32)p + n; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
}

/* the camera on, capturing into buf (CAM_FRAME bytes, 32-aligned, the caller's until cam_stop) */
int cam_start(u8 *buf) {
    if (frame) cam_stop();
    if (sensor_on()) { sensor_off(); return -1; }
    for (u32 a = (u32)buf; a < (u32)buf + CAM_FRAME; a += 32)    /* (no dirty lines left to be written over frames) */
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    vin_reset();
    u32 in = (u32)buf;
    WIN(0x18) = in; WIN(0x24) = in;                       /* the frame (LVC 0x14a50) */
    WIN(0x1c) = WIN(0x20) = WIN(0x28) = WIN(0x2c) = in + CAM_FRAME;   /* (no encoder: nothing beyond the frame) */
    WIN(0x30) = (WIN(0x30) & ~0xffu) | 30;                /* 30 macroblock rows */
    ENG(0x04) = 0x84af1e28; ENG(0x08) = 0x01e00280;       /* 40 x 30 macroblocks, 640 x 480 (LVC 0x14b24, mode 6) */
    VIN(0x00) = (VIN(0x00) & 0xbfffff8fu) | 0x30;
    VIN(0x20) = 0; VIN(0x30) = 0x07ff01f5; VIN(0x34) = 0x07ff07ff;
    ENG(0x74) |= 0x20;
    ENG(0x00) &= 0xdffffff6u;
    VIN(0x28) &= ~0xd900u;                                /* (status only: polled) */
    for (int n = 0; n < 2; ) {                            /* two frames first, as the stock start (state 1) */
        u32 t = ticks_ms;
        while (!(VIN(0x28) & 1) && ticks_ms - t < 200) ;
        if (!(VIN(0x28) & 1)) { cam_stop(); return -2; }
        VIN(0x28) |= 1u << 16; n++;
    }
    REG(0xf0000844u) |= 0x107c; REG(0xf0000844u) &= ~0x107cu;   /* (LVC 0x156fe) */
    frame = buf; cam_frames = 0;
    VIN(0x00) |= 1; state = 1;
    return 0;
}

/* nonzero when a new whole frame is in the buffer; VIN then waits until cam_next() (it never writes while the frame
   is read) */
int cam_ready(void) {
    if (!frame) return 0;
    if (state == 1 && (VIN(0x28) & 1)) { VIN(0x28) |= 1u << 16; VIN(0x00) &= ~1u; state = 2; }
    if (state != 2 || (VIN(0x00) & 2)) return 0;
    state = 0; cam_frames++;
    cache_inval(frame, CAM_FRAME);
    return 1;
}
void cam_next(void) { if (frame && !state) { VIN(0x28) |= 1u << 16; VIN(0x00) |= 1; state = 1; } }

void cam_stop(void) {
    if (!frame) return;
    VIN(0x00) &= ~1u;
    for (u32 t = ticks_ms; (VIN(0x00) & 2) && ticks_ms - t < 200; ) ;   /* (the frame in flight finishes) */
    sensor_off();
    vin_reset();
    VIN(0x28) |= 0xd800;
    frame = NULL; state = 0;
}

/* YUV -> RGB565 (BT.601, studio range): tables, made once */
static short ty[256], tr[256], tg1[256], tg2[256], tb[256];
static u16 cr5[1024], cg6[1024], cb5[1024];              /* clamped and shifted, index + 384 */
static void tables(void) {
    static int done;
    if (done) return;
    for (int i = 0; i < 256; i++) {
        ty[i] = (short)((298 * (i - 16)) >> 8);
        tr[i] = (short)((409 * (i - 128)) >> 8); tg2[i] = (short)((208 * (i - 128)) >> 8);
        tb[i] = (short)((516 * (i - 128)) >> 8); tg1[i] = (short)((100 * (i - 128)) >> 8);
    }
    for (int i = 0; i < 1024; i++) {
        int v = i - 384; v = v < 0 ? 0 : v > 255 ? 255 : v;
        cr5[i] = (u16)((v >> 3) << 11); cg6[i] = (u16)((v >> 2) << 5); cb5[i] = (u16)(v >> 3);
    }
    done = 1;
}

/* the frame in the buffer, 640 x 480, into dst (stride in pixels); mirror: left and right swapped */
void cam_convert(u16 *dst, int stride, int mirror) {
    if (!frame) return;
    tables();
    const u8 *p = frame;
    for (int r = 0; r < 240; r++) {
        u16 *d0 = dst + 2 * r * stride, *d1 = d0 + stride;
        for (int c = 0; c < 40; c++, p += 48) {
            /* pixel i of the unit is at x = 16c + 15 - i; mirrored: 639 - that = 16 (39 - c) + i */
            int x = mirror ? 16 * (39 - c) : 16 * c + 15, step = mirror ? 1 : -1;
            for (int k = 0; k < 8; k++) {
                int cr = p[32 + k], cb = p[40 + k];
                int rv = 384 + tr[cr], gv = 384 - tg1[cb] - tg2[cr], bu = 384 + tb[cb];
                for (int j = 0; j < 2; j++, x += step) {
                    int y0 = ty[p[2 * k + j]], y1 = ty[p[16 + 2 * k + j]];
                    d0[x] = cr5[y0 + rv] | cg6[y0 + gv] | cb5[y0 + bu];
                    d1[x] = cr5[y1 + rv] | cg6[y1 + gv] | cb5[y1 + bu];
                }
            }
        }
    }
}

/* the sensor's 256 registers (shell "cam regs"), set up as for a capture, then off again */
int cam_regs(u8 *out) {
    if (frame) return -1;
    int e = sensor_on();
    for (int i = 0; i < 256 && !e; i++) { out[i] = 0xee; i2c_read(SENSOR, (u8)i, &out[i], 1); }
    sensor_off();
    return e;
}


/* The camera through the H.264 encoder (VENC 0xF0008440, its tables at 0xF0008800), also while the decoder shows
 * video: the stock driver's VGA row mode (drvvcapt: LVC 0x14a50 mode 6 with its row flag, 0x14b24, 0x14d3e configure,
 * 0x156fe start, 0x15866 states, 0x16f30 the capture interrupt).
 *
 * Memory, as the stock lays its block out (LVC 0x6c92a: the camera's part first, the decoder's after it, vdec.c): the
 * output ring, VIN's input (a ring of 3 macroblock rows: WIN +0x30 = 3), the encoder's rolling reference store (a frame
 * and 5 rows: 0x83400) and its side buffer (WIN +0x1C, +0x20). The decoder has the other pair (+0x28, +0x2C: ENG +0x00
 * bit 29, vdec.c), which the camera must not clear (the stock's configure writes it from its row flag).
 *
 * Nothing waits: a wait starves the decoder (it stops for good when the screen refreshes with nothing queued). The
 * sensor is set up a few registers at a time (states 7-9), the engine in one call (state 10); the encoder then goes
 * right at a frame's start, from the capture's interrupt (IRQ 20), after two frames, as the stock (states 11, 12, then
 * the stock's 2-5). Started late in a frame, the engine faults it (ENG +0x70 bit 6).
 *
 * Beside the decoder: a decoder restart (every video keyframe) leaves the encoder running (the decoder stops its own
 * unit only). A fault of either (vdec_fault) resets both: the encoder down here (cam_enc_down), every unit flushed, and
 * the encoder started again once the decoder decodes again (vdec_resets).
 *
 * The output: bare slice data like the decoder's input, in units of 16 bytes left unwritten (the stock sender's packet
 * header) and up to 0x69E bytes. The first frame of every start is a keyframe (an I slice: the capture interrupt asks
 * for it with ENG +0x00 bit 7 for that one frame, LVC 0x170aa); all others are P slices, and nothing in them repairs a
 * lost frame: a receiver that misses one asks for a new start (cam_enc_restart).
 * The reference store must not be written while the decoder runs (filled before a start, the decoder faulted). */
#define ENC_RING  0x285000u                               /* the output ring, to ENC_END */
#define ENC_END   0x291600u
#define ENC_IN    0x291610u                               /* VIN's 3 rows (0xB400) */
#define ENC_REF   (ENC_IN + 0xb400u)                      /* 0x29CA10: the reference store, its side buffer to 0x3255A0 */
#define ENC_UNIT  0x69eu

static int enc_on;
static volatile int enc_state;                            /* 7-9 the sensor, 10 set up, 11-12 two frames, 2-5 the stock's (LVC 0x15866) */
static u32 enc_rp, enc_resets, enc_t;                     /* (enc_resets: vdec_resets when last seen) */
u32 cam_enc_kbps = 3000;                                  /* the encoder's bitrate (the stock's VGA choices: 2000..6000) */
u32 cam_enc_err, cam_enc_starts;                          /* (VENC +0x28 bits 2-3 seen; the encoder's starts) */

static void enc_isr(int n) {                              /* a frame captured (VIN +0x28 bit 0) */
    (void)n;
    u32 st = VIN(0x28);
    if (st & 0xd8) VIN(0x28) |= (st & 0xd8) << 16;        /* (its other events, or the interrupts stop: LVC 0x16fc2) */
    if (!(st & 1)) return;
    VIN(0x28) |= 1u << 16;
    switch (enc_state) {
    case 11: enc_state = 12; break;
    case 12:                                              /* the stock start (LVC 0x156fe) */
        ENG(0x14) = 0;
        REG(0xf0000844u) |= 0x107c; REG(0xf0000844u) &= ~0x107cu;
        ENG(0x70) &= ~0x4000u; REG(0xf0008468u) &= ~0xf0000u; ENG(0x2c) &= ~0x30000u;
        ENG(0x00) |= 1; eng_keep = 1;                     /* the encoder (the bit reads back 0: vdec.c keeps it) */
        VIN(0x00) |= 1;                                   /* the capture, frame after frame */
        enc_rp = ENC_RING; REG(0xf0008460u) = 0;
        enc_state = 2; cam_enc_starts++;
        break;
    case 2:                                               /* the first frame: a keyframe (LVC 0x170aa: ENG +0x00 bit 7) */
        ENG(0x00) |= 0x80; ENG(0x2c) |= 0x22; REG(0xf000886cu) &= ~1u;
        REG(0xf0008810u) = 0x20200420; REG(0xf0008814u) = 8;
        if (VIN(0x00) & 2) { VIN(0x00) &= ~1u; enc_state = 3; }   /* (off for one frame) */
        break;
    case 3: VIN(0x00) |= 1; ENG(0x00) &= ~0x80u; ENG(0x2c) &= ~0x22u; enc_state = 4; break;
    case 4: REG(0xf000886cu) |= 1; if (VIN(0x00) & 2) enc_state = 5; break;
    case 5: REG(0xf000886cu) |= 1; VIN(0x00) |= 1; break; /* (LVC 0x16f4e; armed again every frame) */
    }
}

/* the camera on, encoding: 0, or < 0. Nothing here waits: the sensor's set-up (sensor_on: ~45 ms of I2C) goes on in
   cam_enc_frame, a few registers a call (states 7-9): done at once, it starved the decoder, which asked for a keyframe
   a few seconds into every session (2026-11-24). Frames come ~0.2 s later; cam_enc_frame says < 0 if the sensor
   did not answer. */
int cam_enc_start(void) {
    if (frame || enc_on) return -1;
    memset((void *)ENC_RING, 0, ENC_END - ENC_RING);      /* (2-4 MB is write-through: no cache to clean) */
    cam_enc_err = cam_enc_starts = 0;
    CLOCK = 0x423;                                        /* the sensor's clock: its reset 20 ms later */
    enc_resets = vdec_resets; enc_t = ticks_ms; enc_state = 7; enc_on = 1;
    irq_attach(20, 5, enc_isr);
    return 0;
}

/* the next encoded frame into out (at most max bytes): its length, 0 (none yet) or < 0 (off). Call often. The
   encoder reports each frame's end with a data event (VENC +0x28 bit 1; reading +0x3C gives the end and clears it,
   bit 27 = a frame's end: LVC 0x15f0c); events are acknowledged with bit n + 8 (LVC 0x161d6), or the encoder stops
   after a few frames; bits 2-3 are errors (an overflow: frames not taken in time). */
/* the encoder and the capture off, the encoder's unit flushed; it waits in state 10 to start again. (Left enabled
   without its capture, the encoder held the engine: every decoder start decoded nothing.) */
static void enc_down(void) {
    enc_state = 10;                                       /* (the interrupt does nothing more) */
    VIN(0x00) &= ~1u;
    eng_keep = 0; ENG(0x00) &= ~1u; VIN(0x28) |= 0xd800;
    vin_reset();
    enc_t = ticks_ms;
}
void cam_enc_down(void) { if (enc_on && enc_state < 7) enc_down(); }   /* (vdec_fault) */

int cam_enc_frame(u8 *out, u32 max) {
    if (!enc_on) return -1;
    if (enc_state >= 7 && enc_state <= 9) {               /* the sensor, as sensor_on, in steps */
        static u32 at;
        if (enc_state == 7) { if (ticks_ms - enc_t < 20) return 0; if (sensor_w(0x12, 0x80)) goto dead; enc_t = ticks_ms; enc_state = 8; return 0; }
        if (enc_state == 8) { if (ticks_ms - enc_t < 10) return 0; at = 0; enc_state = 9; return 0; }
        for (int k = 0; k < 4 && at < sizeof vga_yuyv; k++, at += 2) if (sensor_w(vga_yuyv[at], vga_yuyv[at + 1])) goto dead;
        if (at < sizeof vga_yuyv) return 0;
        u8 id = 0;
        i2c_read(SENSOR, 0x0a, &id, 1);
        if (id != 0x77) goto dead;
        enc_resets = vdec_resets; enc_t = ticks_ms; enc_state = 10;
        return 0;
    dead:
        klog("cam: the sensor does not answer\n");
        cam_enc_stop();
        return -1;
    }
    if (enc_resets != vdec_resets) {                     /* the engine was reset (vdec_fault): the encoder again */
        enc_resets = vdec_resets;
        if (enc_state < 7) enc_down();
        return 0;
    }
    if (enc_state == 10) {
        /* beside video: once the decoder decodes again (an encoder going while it waited for its keyframe faulted);
           alone: once the frame in flight has ended (VIN stays busy for good once stopped) */
        if (vdec_active() ? vdec_need_key() : (VIN(0x00) & 2) && ticks_ms - enc_t < 40) return 0;
        enc_down();
        WIN(0x18) = ENC_IN; WIN(0x24) = ENC_IN; WIN(0x1c) = ENC_REF; WIN(0x20) = ENC_REF + 0x83400;
        WIN(0x30) = (WIN(0x30) & ~0xffu) | 3;
        VIN(0x00) = (VIN(0x00) & 0xbfffff8fu) | 0x30;
        VIN(0x20) = 0; VIN(0x30) = 0x07ff01f5; VIN(0x34) = 0x07ff07ff;
        ENG(0x04) = 0x84af1e28; ENG(0x08) = 0x01e00280;   /* 40 x 30 macroblocks, 640 x 480 (LVC 0x14b24, mode 6) */
        REG(0xf0008470u) = ENC_END; REG(0xf000846cu) = ENC_RING + 16;   /* the ring: end, write start (after a unit's
                                                              16-byte header, as the stock B + 16: LVC 0x14dea) */
        /* the rate control (LVC 0x14c4e): a macroblock's share of a frame's bits (kbit/s x 1000 / frames a second /
           1200 macroblocks), that times 43, and 40 << 16 | 43. (0x1C3 here before, a trace's value: 16 Mbit/s at 30
           frames a second, 32 at our 60: no limit at all, and a moving picture's frames flooded the radio.) */
        u32 mb = cam_enc_kbps * 1000 / 71928; mb = mb < 8 ? 8 : mb > 0xfff ? 0xfff : mb;
        REG(0xf0008808u) = mb; REG(0xf000880cu) = mb * 43; REG(0xf0008870u) = 0x0028002b;
        static const u32 q[] = { 0x881c,0x14141414, 0x8820,0x14, 0x8824,0x1a141414, 0x8828,0x14, 0x882c,0x1a14141a, 0x8830,0x14,
            0x8834,0x1a1a141a, 0x8838,0x14, 0x883c,0x201a141a, 0x8840,0x14, 0x8844,0x201a1420, 0x8848,0x14,
            0x884c,0x20201420, 0x8850,0x1a, 0x8854,0x26201a20, 0x8858,0x1a, 0x887c,0x26261a26, 0x8880,0x1a,
            0x8884,0x26262026, 0x8888,0x20, 0x888c,0x2c2c2026, 0x8890,0x20, 0x8894,0x2c2c202c, 0x8898,0x26,
            0x889c,0x322c262c, 0x88a0,0x26, 0x88a4,0x33322632, 0x88a8,0x26, 0x88ac,0x33332c33, 0x88b0,0x2c,
            0x88b4,0x33333333, 0x88b8,0x33 };             /* (the quantiser tables, LVC 0x144e4) */
        for (u32 i = 0; i < sizeof q / sizeof q[0]; i += 2) REG(0xf0000000u + q[i]) = q[i + 1];
        REG(0xf0008478u) = (REG(0xf0008478u) & ~0x7f00u) | 30u << 8;
        REG(0xf0008804u) = 0x485;
        ENG(0x00) &= 0xfffffff6u;                         /* (not bit 29: the decoder's pair) */
        { u32 st = REG(0xf0008468u); (void)REG(0xf000847cu); REG(0xf0008468u) = st | (st & 0xf) << 8; }   /* (stale events) */
        VIN(0x28) &= ~0xd900u;
        enc_state = 11;                                   /* (the rest: enc_isr) */
        return 0;
    }
    if (enc_state > 5) return 0;
    u32 st = REG(0xf0008468u);
    if (st & 0xc) {                                       /* an encoder error (the stock: a fault, LVC 0x15f0c -> 0x15b54) */
        cam_enc_err |= st & 0xc; REG(0xf0008468u) = st | (st & 0xc) << 8;
        if (vdec_active()) vdec_fault("encoder error", st & 0xc); else enc_down();
        return 0;
    }
    if (!(st & 2)) { if (st & 1) REG(0xf0008468u) = st | 1u << 8; return 0; }
    u32 rep = REG(0xf000847cu), end = rep & 0xffffff;     /* (read once: reading clears it) */
    REG(0xf0008468u) = st | (st & 3) << 8;
    /* the ring is [ENC_RING + 16, ENC_END): the encoder wraps to its write start, and the first unit's header is the
       16 bytes before it. Every frame: [16 bytes header][<= 0x69E data]..., from the last frame's end. */
    const u32 base = ENC_RING + 16;
    if (end < base || end >= ENC_END) return 0;
    if (!(rep & 1u << 27)) return 0;                      /* (progress within a big frame: its end comes later) */
    /* (reads of the ring are cached, and the encoder writes RAM behind the cache's back: the cached lines dropped
       first, or the second pass reads the first pass's bytes) */
    if (end >= enc_rp) cache_inval((void *)enc_rp, end - enc_rp);
    else { cache_inval((void *)enc_rp, ENC_END - enc_rp); cache_inval((void *)base, end - base); }
    u32 a = enc_rp, n = 0;
    for (u32 i = 0; a != end && i < ENC_END - ENC_RING; i++) {
        if (i % (ENC_UNIT + 16) >= 16) { if (n < max) out[n] = *(const u8 *)a; n++; }
        if (++a == ENC_END) a = base;
    }
    enc_rp = end;
    REG(0xf0008460u) = enc_rp;                            /* the read pointer: the encoder's room (VENC +0x20) */
    return n <= max ? (int)n : 0;                         /* (too big for out: dropped) */
}

/* the encoder again, from a keyframe (a receiver that lost a frame asks for this: nothing else repairs its picture) */
void cam_enc_restart(void) { if (enc_on && enc_state <= 5) enc_down(); }   /* (off, or starting already: nothing) */

void cam_enc_stop(void) {
    if (!enc_on) return;
    irq_mask(20);
    VIN(0x00) &= ~1u;
    for (u32 t = ticks_ms; (VIN(0x00) & 2) && ticks_ms - t < 200; ) ;
    sensor_off();
    eng_keep = 0; ENG(0x00) &= ~1u;
    vin_reset();
    VIN(0x28) |= 0xd800;
    enc_on = 0;
}
