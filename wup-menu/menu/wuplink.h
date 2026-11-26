#pragma once
#include <stdint.h>
enum { WUP_OFF, WUP_WAITING, WUP_HEADER, WUP_RECEIVING, WUP_READY, WUP_ERROR };
typedef struct { int state, to_file;   /* (to_file: always) */ char name[64]; uint32_t done, total, crc; const char *error; } wup_status_t;
#define WUPLINK_STREAM_PATH "/apps/.wuplink.wup"   /* a large app as received (to_file) */
int wuplink_open(void);          /* listen (TCP 28800) + answer discovery (UDP 28801) */
void wuplink_close(void);
void wuplink_rearm(void);        /* after an error or a launch: wait for the next sender */
void wuplink_step(void);         /* from the menu loop: what arrived, into the file */
const wup_status_t *wuplink_status(void);
int wuplink_port(void);
