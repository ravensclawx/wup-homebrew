/* The wup-homebrew kernel's services, for libwup (built with WUP_KERNEL) and the menu.
 *
 * With the kernel (wup-homebrew/kernel), Nintendo's firmware is stopped at power-on (wup-menu boot/loader.c) and the
 * pad's own drivers run instead: the menu and every app get the whole pad. libwup reaches the kernel through this
 * table, whose address is at WUPK_PTR; the table grows only at its end, and `version` says how far it goes. */
#pragma once
#include <stdint.h>

#define WUPK_PTR     0x00000080u
#define WUPK_MAGIC   0x4b505557u                 /* "WUPK" */
#define WUPK_VERSION 1

typedef struct wupk {
    uint32_t magic, version;
    /* tasks (a lower priority number is more urgent) and counting semaphores */
    int (*task_start)(void (*fn)(void *), void *arg, void *stack, uint32_t size, int prio, const char *name);
    void (*task_exit)(void);
    int (*task_join)(int id, int ms);            /* 0, or -1: timed out */
    void (*task_kill)(int id);                   /* stopped where it is; join it after */
    int (*task_self)(void);
    int (*task_prio)(int id);
    void (*sleep)(int ms);                       /* 0: give others a turn */
    int (*sem_new)(int count, int max);
    void (*sem_free)(int id);
    int (*sem_wait)(int id, int ms);             /* 0, or -1: timed out (ms < 0: for ever, 0: only if free now) */
    void (*sem_signal)(int id);
    /* time and input */
    const volatile uint32_t *ms;                 /* milliseconds since the kernel started */
    const volatile uint8_t *report;              /* the UIC's latest 128-byte input report (as Nintendo's at 0x178D38) */
    const volatile uint32_t *report_seq;         /* counts new reports */
    /* the UIC (the pad's power microcontroller): a command and its answer (battery 0x10, EEPROM 0x03, LCD power 0x12) */
    int (*uic)(const uint8_t *out, int nout, uint8_t *in, int nin);
    void (*backlight)(int pwm);                  /* 0..255 (the panel's own PWM; 122: the console's middle) */
    void (*reboot)(void);                        /* Nintendo's start again: stock, or whatever the buttons pick */
    void (*power_off)(void);
    /* the SPI flash: 0, or < 0 (writes: 4 KB-aligned erases; never below 0xF000) */
    int (*flash_read)(uint32_t addr, void *p, uint32_t n);
    int (*flash_write)(uint32_t addr, const void *p, uint32_t n);   /* program (the sectors erased first) */
    int (*flash_erase4k)(uint32_t addr);
    /* Wi-Fi: the radio up (its firmware from Nintendo's bank), Broadcom's ioctls and iovars, Ethernet frames */
    int (*wifi_up)(void);
    int (*wifi_ioctl)(uint32_t cmd, void *buf, uint32_t len, int set);
    int (*wifi_var)(const char *name, void *v, uint32_t n, int set);
    int (*wifi_join)(const char *ssid, const uint8_t *pmk);         /* pmk: 32 bytes (WPA2), or NULL (open) */
    void (*wifi_leave)(void);
    int (*wifi_state)(void);                     /* 0 off, 1 joining, 2 connected, 3 failed, 4 wrong password */
    int (*wifi_tx)(const void *eth, uint32_t len);
    int (*wifi_tx_room)(void);
    void (*wifi_hooks)(void (*rx)(const uint8_t *eth, uint32_t len), void (*link)(int up));
    const uint8_t *mac;                          /* the radio's MAC, once it is up */
    /* the log (the UART) */
    void (*log)(const char *s);
} wupk_t;

#define WUPK (*(const wupk_t *const *)WUPK_PTR)
static inline int wupk_present(void) { return WUPK && WUPK->magic == WUPK_MAGIC; }
