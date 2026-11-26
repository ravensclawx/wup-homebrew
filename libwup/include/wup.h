/* libwup: the runtime for GamePad apps (.wup) on wup-homebrew, started from the Homebrew Menu.
 *
 * Write   WUP_APP("My app");   and   int main(void) { ... return 0; }
 * Build   python3 tools/wupbuild.py <dir or .c files> --name "My app" [--icon icon.png]   -> a .wup
 *
 * The pad runs the wup-homebrew kernel (wup-homebrew/kernel): nothing of Nintendo's runs while an app does. Only the
 * kernel stays in memory between apps (include/handoff.h); the menu is loaded again when main() returns.
 * Memory: the app owns one flat block, WUP_APP_BASE..WUP_APP_END (about 1.8 MB, include/wup_ram.h): its code and data,
 * its .bss, then the heap, malloc(), to the end. The C library is picolibc: <stdio.h>, <stdlib.h>, <string.h>,
 * <math.h>...; printf goes to the log; fopen and friends work on the pad's filesystem (sys.c). Threads: <pthread.h>.
 * Display: the RGB565 screen (wup_screen, include/gfx.h drawing and fonts, or LVGL: include/wup_lvgl.h), double
 * buffered by the kernel; or an 8-bit paletted screen (wup_screen8: 854x480 bytes, 256 colours).
 * Every app fades in once its first frame is shown (gfx_present, gfx_present_all or wup_present8; or after 0.3 s) and
 * fades out when main() returns: nothing to do in the app. The volume slider and the Power button work in every app
 * (the kernel). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "gfx.h"
#include "fs.h"
#include "handoff.h"

#include "wup_ram.h"
#define WUP_APP_SIZE      (WUP_APP_END - WUP_APP_BASE)

#define WUP_APP(title) \
    int _wup_entry(void); \
    __attribute__((section(".header"), used)) const struct { uint32_t magic, entry, size; char name[32]; \
                                                             uint32_t reserved[3]; } \
    _wup_header = { 0x57524248u, (uint32_t)_wup_entry, 0, title, { 0, 0, 0 } }   /* (size: wupbuild.py) */
int main(void);

/* start another app when this one ends: check the .wup now (0 = fine), then return from main(). arg: a file for it
   to open (its wup_arg()), or NULL. The app after that one is the menu again. */
int wup_exec(const char *wup_path, const char *arg);
const wup_handoff_t *wup_handoff(void);                     /* the menu's hand-off (app path, argument) */
/* Where the app lives, like argv[0] on the Switch: "/apps/game_boy/game_boy.wup". Keep the app's data
   in its folder (wup_app_dir: "/apps/game_boy"), so deleting the folder removes everything. */
const char *wup_app_path(void);
const char *wup_app_dir(void);
const char *wup_arg(void);                           /* a file to open (argv[1]), or "" */
int wup_inflate(void *out, unsigned *out_len, const void *in, unsigned in_len);   /* raw deflate (a zip entry): 0, *out_len its size */
int wup_fs_ok(void);                                 /* the filesystem mounted (else fs_* fail) */

/* RGB565 full screen (854x480): draw with gfx_* (include/gfx.h), then gfx_present()/gfx_present_all() */
px_t *wup_screen(void);
/* the 8-bit paletted screen: 854x480, stride wup_stride8(); colours from wup_palette() */
uint8_t *wup_screen8(void);
uint32_t wup_stride8(void);
void wup_palette(int index, uint32_t rgb);           /* 0xRRGGBB */
void wup_present8(int y, int h);                     /* show rows y..y+h (cache clean) */
void wup_vblank(void);                               /* wait for the vertical blanking interval */

/* input */
enum { WUP_A = 0x8000, WUP_B = 0x4000, WUP_X = 0x2000, WUP_Y = 0x1000, WUP_LEFT = 0x0800, WUP_RIGHT = 0x0400,
       WUP_UP = 0x0200, WUP_DOWN = 0x0100, WUP_ZL = 0x0080, WUP_ZR = 0x0040, WUP_L = 0x0020, WUP_R = 0x0010,
       WUP_PLUS = 0x0008, WUP_MINUS = 0x0004, WUP_HOME = 0x0002 };
unsigned wup_buttons(void);
/* sticks: 0 at rest (each pad rests a little off centre: measured when the app starts), about +-WUP_STICK_MAX at
   full tilt (measured on the pad), right and up positive */
#define WUP_STICK_MAX 1100
int wup_stick(int which, int axis);                  /* which 0 left / 1 right, axis 0 x / 1 y */
int wup_touch(int *x, int *y);                       /* 1 while the screen is touched: x 0..853, y 0..479 */

/* audio: 48 kHz 16-bit stereo, the GamePad speakers / headphones (volume slider applies). wup_audio_queue() appends
   frames (L,R pairs) to what is playing, up to about 0.2 s ahead; a sound queued after silence starts 33 ms after the
   call. Usable from any thread; a thread of libwup's own keeps the hardware's ring clean while it is open. */
int wup_audio_open(void);                            /* 0 = playing */
int wup_audio_queue(const int16_t *lr, int frames);  /* how many it took (fewer when there is no room: queue the rest later) */
int wup_audio_queued(void);                          /* frames still waiting to be played */
int wup_audio_room(void);                            /* frames that can be queued now */
void wup_audio_close(void);

/* the app fades in with its first presented frame, or after 0.3 s whatever is on screen; wup_screen_hold()
   waits up to 3 s for that first frame (an app preparing a complete first screen) */
void wup_screen_hold(void);
void wup_screen_instant(void);                     /* the first frame appears at once instead of fading in (the menu) */

/* the rumble motor, the screen's brightness and the microphone (all put back when main() returns) */
void wup_rumble(int on);                             /* the motor on / off (it is one speed) */
void wup_backlight(int percent);                     /* 0..100 (0: dark) */
int wup_backlight_level(void);                       /* the menu's (Settings > Brightness), 0..100 */
int wup_mic_open(void);                              /* 16-bit mono 48 kHz; 0 = recording */
int wup_mic_read(int16_t *out, int max);             /* samples recorded since the last call: call at least every 20 ms */
void wup_mic_close(void);

/* time */
/* the date and time: known once the menu has had them from the network (each time it joins Wi-Fi); then time(),
   gettimeofday() and localtime() give them, in the time zone set in the menu. The pad keeps no time when it is off. */
int wup_time_known(void);
void wup_time_set(int64_t unix_seconds, uint32_t us);   /* (the menu) */
void wup_time_zone(const char *posix_tz);               /* (the menu) e.g. "PST8PDT,M3.2.0,M11.1.0" */
uint32_t wup_us(void);                               /* free-running ~1 MHz clock (wraps) */
void wup_sleep(int ms);                              /* gives the other tasks a turn (and runs net = raw networking) */
void wup_log(const char *s);                         /* debug output: the UART, and with networking up also
                                                        UDP broadcast port 28803 (tools/wuplog.py shows it) */
void wup_log_hex(uint32_t v);

/* status, as the Homebrew Menu shows it */
int wup_battery(int *charging);                      /* percent (0..100), or -1 unknown; charging: 1 while charging */
int wup_wifi_rssi(void);                             /* the connection's signal in dBm (< 0), or 0 when not connected */

/* Threads, and what they wait on: the wup-homebrew kernel's (kernel.c). <pthread.h> is built on
   these. Threads of one priority do not take turns by themselves: one runs until it waits (a semaphore, a sleep,
   a blocking socket call). Everything is given back when main() returns: end your threads before that.
   priority: 0 = as the caller, +1 more urgent (it runs whenever it can), -1 less. */
int wup_thread_start(void (*fn)(void *), void *arg, unsigned stack_size, int priority);   /* a handle (>= 0), or < 0 */
#define WUP_THREADS 12                               /* at most, besides the main one */
void wup_thread_detach(int thread);                  /* nobody will join it: it is cleared away when it has ended */
void wup_thread_kill(int thread);                    /* stop it where it stands (for one that never returns; what it holds stays held) */
int wup_thread_join(int thread, int ms);             /* wait for it to return (ms < 0: for ever): 0, or < 0 not yet */
int wup_thread_self(void);                           /* the handle of the running thread; -1: the main one */
void wup_thread_sleep(int ms);                       /* in a thread: a plain sleep (wup_sleep, for the main one, runs the network too) */
int wup_sem_new(int count, int max);                 /* a counting semaphore: its handle (> 0), or < 0 */
void wup_sem_free(int sem);
int wup_sem_wait(int sem, int ms);                   /* take one (ms < 0: wait for ever, 0: only if there is one): 0, or < 0 */
void wup_sem_signal(int sem);                        /* give one */
unsigned wup_irq_off(void);                          /* a few instructions that nothing may interrupt: off ... */
void wup_irq_restore(unsigned state);                /* ... and as it was */

/* network (app.cfg: net = yes): BSD sockets, lwIP's own, with the stack in threads of its own. The menu keeps Wi-Fi
   connected; wup_net_init() takes its connection over (no DHCP, no Wi-Fi code in the app). Then use <sys/socket.h>,
   <netdb.h>, <poll.h>, <arpa/inet.h> as on any other system: socket, connect, send, recv, select, getaddrinfo...
   from any thread. 127.0.0.1 works even without Wi-Fi. Everything is closed when main() returns.
   (net = raw: lwIP's callback API, polled from wup_sleep(), no sockets and no threads: the menu.) */
int wup_net_init(void);                              /* 0, or < 0: the menu was not connected to Wi-Fi */
int wup_net_start(const uint8_t mac[6]);             /* (the menu) the stack on the radio, no address yet */
int wup_net_dhcp(void);                              /* (the menu) ask the joined network for an address */
void wup_net_leave(void);                            /* (the menu) the address given back: the network is being left */
void wup_net_info(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns);
int wup_net_online(void);
uint32_t wup_net_ip(void);                           /* the pad's address (network byte order) */
const char *wup_net_ip_str(void);                    /* "192.168.4.101" */
int wup_net_stats(char *out, int n);
void wup_net_time(void);                             /* the date and time from the network (SNTP), once networking is up */
void wup_net_tx_pacing(unsigned frames_per_ms_x10, unsigned burst);   /* (diagnostics) */                 /* "rx=... rx_dropped=... pool_err=..." (diagnostics) */
extern void (*wup__net_poll)(void), (*wup__net_exit)(void), (*wup__net_log)(const char *);   /* (libwup internal) */
extern int (*wup__sock_close)(int fd);
