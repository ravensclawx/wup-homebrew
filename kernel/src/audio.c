/* Sound: the audio block (0xF0005400) playing 16-bit stereo at 48 kHz through the codec (a TI TLV320AIC3012 on
 * I2C 0x18) to the speaker.
 *
 * Bring-up: the stock firmware's own, replayed (fw_audio.c, tools/mkaudio.py): the codec's set-up, the audio block's
 * format and clocks. Playing: an earlier homebrew library's way, proven on the pad under the stock firmware (an earlier homebrew library): the block
 * loops over one ring (write pointer = ring end) at 192000 bytes a second; samples are written just ahead of the play
 * position, and what has been played is cleared at once (the 1 kHz tick, audio_tick), or the ring would play it again
 * 0.24 s later. +0x14 is the play position, +0x20 bit 0 mutes (bit 2, stop, wedges an idle engine: never used).
 * The ring sits in the write-through 2-4 MB (mmu.c): what the CPU writes is in RAM for the block's DMA.
 *
 * Volume: the slider (input byte 14, 0..255) sets the DAC's digital volume (page 0 reg 0x41, signed half-dB; reg 0x40
 * bits 2-3 mute, bit 1 right follows left: the firmware leaves 0x02). The curve is ours (inferred): 40 log10(s / 255)
 * dB, which gives the -5 dB (0xF6) the firmware set at the emulator's 75 %. */
#include "os.h"
#include "fw_audio.h"

#define AU(o)    REG(0xf0005400u + (o))
#define CODEC    0x18
#define RING     ((u32 *)AUDIO_RING)
#define RFRAMES  11520                           /* 0.24 s */
#define RBYTES   (RFRAMES * 4)
#define LEAD     1600                            /* a new sound starts this far ahead of the play position (33 ms) */
#define ROOM     (RFRAMES - LEAD - 480)          /* never more queued than this */

static int on, clr, wr = -1, left;               /* wr < 0: nothing queued */
static int volume = -1;

static int codec(u8 reg, u8 v) { u8 b[2] = { reg, v }; return i2c_write(CODEC, b, 2); }

static int pos(void) { u32 p = (AU(0x14) & 0xffffff) - (u32)RING; return p < RBYTES ? (int)(p >> 2) : -1; }
static int dist(int a, int b) { int d = b - a; return d < 0 ? d + RFRAMES : d; }

int audio_init(void) {
    for (int i = 0; i < fw_audio_n; i++) {
        const fw_audio_op *o = &fw_audio[i];
        if (o->kind == 1) { REG(o->a) = o->v; continue; }
        u8 b[9] = { (u8)o->a };
        for (u32 k = 0; k < o->v && k < 8; k++) b[1 + k] = o->b[k];
        if (i2c_write(CODEC, b, (int)o->v + 1)) { klog("audio: the codec does not answer (write %d)\n", i); return -1; }
        if (o->a == 0x01 && o->b[0] == 1) delay_ms(10);   /* (its software reset: inferred settle time) */
    }
    AU(0x2c) = 0; AU(0x30) = 0x3f;                        /* no interrupts: we keep the ring ourselves */
    AU(0x20) = (AU(0x20) & ~4u) | 1;                      /* muted while we set up */
    memset(RING, 0, RBYTES);
    AU(0x00) &= ~(7u << 22);                              /* PCM16, 48 kHz */
    AU(0x04) &= ~0x80u;                                   /* stereo */
    AU(0x08) = (u32)RING; AU(0x0c) = (u32)RING + RBYTES;
    AU(0x24) = RBYTES >> 4;
    AU(0x10) = (u32)RING + RBYTES;                        /* loop over the ring */
    delay_ms(4); int p0 = pos(); delay_ms(4); int p1 = pos();
    if (p0 < 0 || p1 < 0 || p0 == p1) { klog("audio: the block does not play (%d, %d)\n", p0, p1); return -2; }
    clr = p1; wr = -1; left = 0;
    on = 1;
    AU(0x20) &= ~5u;
    klog("audio: on\n");
    return 0;
}

/* (the 1 kHz tick) clear what was played; notice the end of the queue */
void audio_tick(void) {
    if (!on) return;
    int p = pos();
    if (p < 0) return;
    while (clr != p) { RING[clr] = 0; if (++clr == RFRAMES) clr = 0; }
    if (wr >= 0) {
        int d = dist(p, wr);
        if (d > left) wr = -1;                            /* the play position has passed the end */
        else left = d;
    }
}

int audio_queued(void) { if (!on) return 0; irq_off(); audio_tick(); int n = wr < 0 ? 0 : left; irq_on(); return n; }
int audio_room(void) { return on ? ROOM - audio_queued() : 0; }

/* n stereo frames (left, right, 16 bits each); returns how many were taken */
int audio_queue(const short *lr, int n) {
    if (!on || n <= 0) return 0;
    irq_off();
    audio_tick();
    int p = pos();
    if (p < 0) { irq_on(); return 0; }
    if (wr < 0) { wr = p + LEAD >= RFRAMES ? p + LEAD - RFRAMES : p + LEAD; left = LEAD; }
    if (n > ROOM - left) n = ROOM - left;
    const u32 *s = (const u32 *)lr;
    for (int i = 0; i < n; i++) { RING[wr] = s[i]; if (++wr == RFRAMES) wr = 0; }
    left += n;
    irq_on();
    return n;
}

/* everything queued dropped (an app ended) */
void audio_stop(void) {
    if (!on) return;
    irq_off();
    memset(RING, 0, RBYTES);
    wr = -1; left = 0;
    irq_on();
}

/* the volume slider (0..255) to the DAC; only when it moved */
void audio_volume(int slider) {
    static const s8 curve[32] = { -128, -120, -96, -82, -72, -64, -58, -53, -48, -44, -40, -37, -34, -31, -29, -26,
                                  -24, -22, -20, -18, -16, -14, -13, -11, -10, -8, -7, -6, -5, -3, -2, -1 };
    if (!on || (volume >= 0 && (slider - volume < 3 && volume - slider < 3))) return;
    volume = slider;
    int i = slider >> 3, f = slider & 7, a = curve[i], b = i < 31 ? curve[i + 1] : 0;
    int hdb = a + (b - a) * f / 8;
    codec(0x00, 0x00);                                    /* page 0 */
    if (slider < 4) { codec(0x40, 0x0e); return; }        /* (the firmware's mute: both DACs, right follows left) */
    codec(0x41, (u8)(s8)(hdb < -127 ? -127 : hdb));
    codec(0x40, 0x02);
}

/* ---- the microphone: the capture block (0xF0005480; the stock firmware's drvacapt, LVC 0x2000-0x2e00; an earlier homebrew library's
   hw_mic proved it on the pad): 16-bit mono, 48 kHz, into a ring: +0x20 start, +0x24 end, +0x28 the read position,
   +0x2C how far into the ring the block has written, all in 16-byte units. Started as the stock start routine does
   (LVC 0x256a): a reset pulse (0xF0005444 bit 31), the read position to the start, bit 0 to run. The codec's
   microphone side is part of the replayed bring-up (fw_audio.c). The ring is in RAM the block writes behind the
   CPU's back: the cache lines are dropped before each read. */
#define CAP(o)   REG(0xf0005480u + (o))
#define CAPCTL   REG(0xf0005444u)
#define MBYTES   0x10000u                        /* 64 KB: 0.68 s */
static u32 mic_on, mic_rd;

int mic_open(void) {
    if (mic_on) return 0;
    if (!on) return -1;                                   /* (the codec: audio_init) */
    CAP(0x20) = MIC_RING >> 4; CAP(0x24) = (MIC_RING + MBYTES) >> 4;
    CAPCTL |= 0x80000000u; CAPCTL &= ~0x80000000u;
    CAP(0x28) = CAP(0x20);
    mic_rd = MIC_RING;
    CAPCTL |= 1;
    mic_on = 1;
    return 0;
}
void mic_close(void) { if (mic_on) { CAPCTL &= ~1u; mic_on = 0; } }

/* up to max samples recorded since the last call: how many */
int mic_read(short *out, int max) {
    if (!mic_on) return 0;
    u32 a = MIC_RING, e = MIC_RING + MBYTES, w = a + (CAP(0x2c) << 4), got = 0;
    if (w >= e || mic_rd < a || mic_rd >= e) return 0;
    while (mic_rd != w && got < (u32)max) {
        u32 n = ((w > mic_rd ? w : e) - mic_rd) / 2;
        if (n > (u32)max - got) n = (u32)max - got;
        for (u32 l = mic_rd & ~31u; l < mic_rd + n * 2; l += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(l) : "memory");
        const volatile s16 *s = (const volatile s16 *)mic_rd;
        for (u32 i = 0; i < n; i++) out[got + i] = s[i];
        got += n; mic_rd += n * 2;
        if (mic_rd == e) mic_rd = a;
    }
    CAP(0x28) = mic_rd >> 4;                              /* (how far we have read) */
    return (int)got;
}
