/* The H.264 decoder (VDEC 0xF0008480, the codec engine ENG 0xF0008C00) and the display's video layer: the decoder
 * writes its pictures into its own memory (ram.h) and the video layer shows them; the CPU never touches a pixel.
 * From the stock firmware (drvvout, LVC 0x17400-0x18a00) and the old wupstream receiver (wup-homebrew/wup-stream
 * pad/main.c, proven on the pad under the stock firmware).
 *
 * Input: each frame's raw CABAC slice data, as the GamePad's decoder takes it (no start codes, NAL headers or
 * emulation prevention: libwupstream's x264, patched for the pad, makes such frames). Frames go into the bitstream
 * ring (VDEC +0x24 .. +0x28); the decoder's own queue (VDEC +0x00, count in +0x34) holds 4; every keyframe restarts
 * the decoder (it cannot find one otherwise: there are no slice headers; so does the stock, LVC 0x1811e). It stops for
 * good when the screen refreshes with nothing queued: a lost or late frame stops it at once instead (the stock's error
 * stop, LVC 0x17b5e) and only a keyframe starts it again (vdec_need_key).
 *
 * Beside the camera's encoder (camera.c), the stock's "row mode" (2026-11-24), used always here:
 * - the engine has two picture stores, each a pair in the window block: its base and a small side buffer (0xF0000800
 *   +0x1C, +0x20 and +0x28, +0x2C). The encoder always takes the first. ENG +0x00 bit 29 puts the decoder on the
 *   second and makes it wait for rows (the stock sets it in the camera's configure, LVC 0x14d3e):
 * - VDEC +0x38 (low byte) counts the macroblock rows queued: + 30 with every frame (LVC 0x1810a);
 * - stopping takes the decoder's own unit only (ENG +0x74 bit 9, not 0x21A: LVC 0x17ac0), starting unmasks bit 12 too.
 * In its frame mode (bit 29 clear) the decoder holds the units the encoder needs: the encoder faults at once. In
 * row mode a decoder restart leaves the encoder running; a fault of either resets both (vdec_fault, as the stock's
 * camera and video drivers do together: LVC 0x15b54, 0x17838, 0x1a5fc).
 * Memory as the stock lays it out, the camera's part first (LVC 0x175a8, 0x6c92a): the picture store (a frame and 5
 * macroblock rows, rolling: 0xB1300), then the bitstream ring (0x27A70). The side buffer's address is the RAM's end
 * (the stock's choice: nothing is written there). */
#include "os.h"

#define VDEC(o)  REG(0xf0008480u + (o))
#define ENG(o)   REG(0xf0008c00u + (o))
#define WIN(o)   REG(0xf0000800u + (o))
#define LCD(o)   REG(0xf0009400u + (o))
#define RST844   REG(0xf0000844u)
#define STORE    0x3255a0u                                /* the picture store, then the ring (to 0x3FE310) */
#define RING     (STORE + 0xb1300u)
#define RING_END (RING + 0x27a70u)

static u32 rbase, rsize, wp, on, need_key = 1, pend[8], npend, lcd80, shown, wd_rp, wd_n, wd_ms;
u32 vdec_frames, vdec_keys, vdec_stalls, vdec_resets;
u32 eng_keep;                                             /* the encoder's enable (ENG +0x00 bit 0) reads back 0: kept in our writes */

/* the decoder's own unit stopped (LVC 0x17a10, row mode): the camera's encoder runs on through it (proven 2026-11-24:
   10 decoder restarts, the encoder never stopped, its stream clean). The stock stops the decoder this way at once on
   every error (a lost or bad packet: LVC 0x17b5e), never lets it starve. */
static void stop_engine(void) {
    ENG(0x00) = ENG(0x00) | 0x20000 | eng_keep;
    ENG(0x74) |= 0x200;
    for (int i = 0; i < 20 && !(ENG(0x74) & 0x200); i++) delay_ms(1);
    ENG(0x74) |= 0x80;
    ENG(0x74) &= ~0x280u;
    VDEC(0x38) = 0;
}
/* Every unit of the engine flushed (LVC 0x1a5fc, 0x14806), and its faults acknowledged (ENG +0x70 bit n + 16: LVC
   0x16226): after a fault the row-mode stop is not enough (the decoder stays "running", ENG +0x00 bit 18, without
   decoding a row; the encoder faults at every start, in every later session too). It stops the encoder as well. */
static void flush_all(void) {
    ENG(0x00) = ENG(0x00) | 0x20000 | eng_keep;
    ENG(0x74) |= 0x21e;
    for (int i = 0; i < 20 && (ENG(0x74) & 0x21e) != 0x21e; i++) delay_ms(1);
    ENG(0x74) |= 0xc0;
    ENG(0x74) &= 0xfffffd21u;
    ENG(0x70) |= (ENG(0x70) & 0xfc) << 16;
    VDEC(0x38) = 0;
}
static void start_engine(void) {                          /* LVC 0x17ee0 (row mode; force decoding on, as the stock state) */
    VDEC(0x0c) = 3;
    ENG(0x70) &= ~0xb000u;
    VDEC(0x30) |= 0x00ff0000; VDEC(0x30) &= ~0x00030000u;
    if (!(LCD(0x84) & 4)) { ENG(0x20) = 0; RST844 |= 0x00300000; RST844 &= ~0x00300000u; }
    RST844 |= 0x30000000; RST844 &= ~0x30000000u;
    ENG(0x00) = ENG(0x00) | 0x10000 | eng_keep;
}
/* the decoder off until the next keyframe (a frame lost or not taken in time: the stock's error stop, LVC 0x17b5e) */
static void lose(void) { if (on) stop_engine(); on = 0; need_key = 1; npend = 0; }

/* A fault of the engine's (ENG +0x70 bits 3, 6: the encoder's; bits 4, 7: the decoder's), a decoding error (VDEC +0x30
   bit 1) or an encoder error (VENC +0x28 bits 2-3): the stock's reset of both (row mode, LVC 0x15b54 -> 0x17838 ->
   0x1a5fc, 0x17a10 -> notify 0x13): the camera's encoder down, every unit flushed, the decoder off until a keyframe,
   then the camera started again (camera.c sees vdec_resets). */
void vdec_fault(const char *what, u32 bits) {
    klog("vdec: %s %02x: the engine reset\n", what, bits);
    cam_enc_down();
    flush_all();
    VDEC(0x30) |= 0x200;                                  /* (the decoding error acknowledged) */
    on = 0; need_key = 1; npend = 0;
    vdec_resets++;
}
static void queue(u32 entry) { VDEC(0x00) = entry; VDEC(0x38) = (VDEC(0x38) + 30) & 0xff; }   /* a frame, its 30 rows */

/* room in the ring for a new frame (bytes) */
static u32 ring_free(void) {
    if (!on) return rsize - 16;
    u32 rp = (VDEC(0x2c) & 0xffffff) - rbase;
    if ((rp + rsize - wp) % rsize < 4096) return rsize - 4096;   /* (it reads a little past the end: just ahead = empty) */
    u32 used = (wp + rsize - rp) % rsize;
    return used + 4096 < rsize ? rsize - used - 4096 : 0;
}
static void feed(void) { while (npend && (VDEC(0x34) & 7) < 4) { queue(pend[0]); npend--; for (u32 i = 0; i < npend; i++) pend[i] = pend[i + 1]; } }

int vdec_open(void) {
    flush_all();                                          /* (whatever the last session left) */
    WIN(0x28) = STORE; WIN(0x2c) = 0x400000;              /* the second pair (the first is the camera's) */
    VDEC(0x24) = RING; VDEC(0x28) = RING_END;
    ENG(0x74) |= 0x20;                                    /* (LVC 0x18214) */
    ENG(0x00) = ENG(0x00) | 0x20000000u | eng_keep;       /* row mode, the second pair */
    /* the picture store: black (units of 48 bytes: 32 of luma, 16 of chroma): the layer shows it for a moment before the
       first picture is decoded, and what an earlier session or the framebuffers left there was a flicker at every start */
    for (u8 *p = (u8 *)STORE; p < (u8 *)RING; p += 48) { memset(p, 0x10, 32); memset(p + 32, 0x80, 16); }
    rbase = RING; rsize = RING_END - RING;
    wp = 0; on = 0; need_key = 1; npend = 0; shown = 0;
    vdec_frames = vdec_keys = vdec_stalls = 0;
    return 0;
}

/* one frame (keyframe: idr nonzero); 0, or < 0: dropped (not started, or no room: send a keyframe) */
int vdec_frame(const void *data, u32 len, int idr) {
    if (!rsize) return -1;
    if (!idr && (need_key || !on)) return -1;
    if (!len || len > rsize - 4096 || (!idr && len > ring_free())) { lose(); return -1; }
    if (!idr && npend == 8) { lose(); vdec_stalls++; return -1; }
    u32 start = idr ? 0 : wp, first = len < rsize - start ? len : rsize - start;
    memcpy((u8 *)(rbase + start), data, first);           /* (write-through RAM: the decoder sees it) */
    if (len > first) memcpy((u8 *)rbase, (const u8 *)data + first, len - first);
    u32 end = rbase + (start + len) % rsize;
    if (idr) {                                            /* the stock stream-start: stop, the ring from here, start */
        if (on) stop_engine();
        ENG(0x70) |= (ENG(0x70) & 0x24) << 16;            /* (its stop's statuses: bit 5 "stopped", bit 2) */
        VDEC(0x0c) = 0;
        u32 b = VDEC(0x24);
        VDEC(0x24) = 0; VDEC(0x24) = (rbase + start) & 0xffffff; VDEC(0x24) = b;
        queue((end & 0xffffff) | 1u << 24);
        start_engine();
        on = 1; npend = 0; vdec_keys++;
    } else { pend[npend++] = (end & 0xffffff) | 1u << 24; feed(); }
    wp = (start + len) % rsize; need_key = 0; vdec_frames++;
    if (!shown) { lcd80 = LCD(0x80); LCD(0x80) = (lcd80 | 4) & ~0x10u; shown = 1; }   /* the video layer on, ours off (with
                                                             the first frame: the decoder runs with the display: its layer
                                                             off, it stalls after a frame) */
    return 0;
}

/* call often: the queue, and the stall check (60 ms without progress while frames wait) */
void vdec_poll(void) {
    if (!rsize) return;
    u32 f = ENG(0x70) & 0xd8;
    /* (bit 4: the stock treats it as the decoder's fault; here, fed by a stream that arrives late now and then, it
       comes dozens of times a minute and the picture goes on: acknowledged only; a reset on it reset the engine forty
       times a minute. Our one known difference from the stock.) */
    if (f & 0x10) { ENG(0x70) |= 0x100000; f &= ~0x10u; }
    if (f) { vdec_fault("engine fault", f); return; }
    if (on && (VDEC(0x30) & 2)) { vdec_fault("decoding error", VDEC(0x30) & 0xff); return; }
    if (!on) return;
    feed();
    u32 rp = VDEC(0x2c) & 0xffffff, n = VDEC(0x34) & 7;
    if (rp != wd_rp || n != wd_n || !n) { wd_rp = rp; wd_n = n; wd_ms = ticks_ms; return; }
    if (ticks_ms - wd_ms > 60) { lose(); wd_ms = ticks_ms; vdec_stalls++; }
}
int vdec_need_key(void) { return need_key; }
int vdec_active(void) { return rsize != 0; }           /* open (between vdec_open and vdec_close) */
u32 vdec_waiting(void) { u32 n = (VDEC(0x34) & 7) + npend; return n > 7 ? 7 : n; }

void vdec_close(void) {
    flush_all();
    ENG(0x00) = (ENG(0x00) & ~0x20000000u) | eng_keep;    /* (the engine as the boot left it) */
    on = 0; need_key = 1; npend = 0;
    if (shown) { LCD(0x80) = lcd80; shown = 0; }          /* our layer back (its framebuffers were the decoder's: redraw) */
    rsize = 0;
}
