/* The pad's input: buttons, sticks, touch, motion (sys.c reads the UIC's report into it). */
#pragma once
#define PAD_W 854
#define PAD_H 480

/* buttons: pad_input_t.held / .pressed */
enum {
    PAD_A = 0x8000, PAD_B = 0x4000, PAD_X = 0x2000, PAD_Y = 0x1000,
    PAD_LEFT = 0x0800, PAD_RIGHT = 0x0400, PAD_UP = 0x0200, PAD_DOWN = 0x0100,
    PAD_ZL = 0x0080, PAD_ZR = 0x0040, PAD_L = 0x0020, PAD_R = 0x0010,
    PAD_PLUS = 0x0008, PAD_MINUS = 0x0004, PAD_HOME = 0x0002, PAD_SYNC = 0x0001,
    PAD_L3 = 0x10000, PAD_R3 = 0x20000, PAD_TV = 0x40000,
};

typedef struct {
    unsigned held, pressed;                      /* buttons down now; down now but not in the frame before */
    short lx, ly, rx, ry;                        /* sticks, about -1100..1100, up and right positive */
    unsigned char volume;                        /* the volume slider, 0..255 */
    unsigned char battery, charging;             /* 0..6 */
    unsigned char touch;                         /* the screen is touched: tx, ty in pixels */
    short tx, ty;
    /* ABI 3: motion, raw from the report (accelerometer: 16 bits, bytes 15-20; gyroscope: 24 bits, 21-29;
       magnetometer: 16 bits, 30-35; all three respond on the pad) */
    short accel[3];
    int gyro[3];
    short mag[3];
    /* ABI 4 */
    unsigned repeat;                             /* pressed, plus the D-pad repeating while held (400 ms, then every
                                                    80 ms): for menus, lists, keyboards. Games use pressed */
} pad_input_t;

