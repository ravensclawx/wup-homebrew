/* dbgsrv: the UART monitor's protocol over TCP (port 28802), served by the Homebrew Menu, so the host
 * tools (tools/flash/wupmon.py --port tcp:IP) work over Wi-Fi without the UART cable.
 * Same commands and replies as the original UART monitor (tools/wupmon.py): ping info r8/r16/r32 w8/w16/w32 rb wb fr call sleep.
 * Differences over TCP: wb streams its LEN bytes straight after "GO" (no per-64-byte '.' acks) into
 * the target, then "OK" / "ERR crc"; rb/fr send live memory and checksum exactly what was sent.
 * Received bytes are only queued in the lwIP callback; commands run from dbgsrv_step() in the menu's
 * loop (outside lwIP), which may poll the network itself while it waits to send.
 * Anyone on the network can read/write/call anything: it is a development tool (Settings switch). */
#include "wupk.h"
#include <stdint.h>
#include <string.h>
#include "lwip/tcp.h"
#include "dbgsrv.h"
#include "menu.h"
#include "armcpu.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#include "wup.h"
static u8 FLBUF[4096] __attribute__((aligned(4)));   /* (a flash read, 4 KB at a time) */
#include "wupk.h"
#define DLY(ms) WUPK->sleep(ms)
#define MAXLEN 0x20000u

static struct tcp_pcb *listener, *conn;
static char inq[2048]; static u32 inq_n;
static int wb_active; static u8 *wb_dst; static u32 wb_left, wb_len, wb_crc, wb_want;
static u8 fw_buf[4096] __attribute__((aligned(4))); static u32 fw_addr, fw_on;   /* an fw: one sector, then flashed */
static u32 ncmds;


/* ---- output: blocking from the menu loop, polling the network until lwIP takes everything */
static void out(const void *p, u32 n) {
    const u8 *d = p;
    u32 t = 0;
    while (n && conn) {
        u32 room = tcp_sndbuf(conn);
        if (!room) { tcp_output(conn); wup_sleep(1); if (++t > 5000) return; continue; }
        u32 k = n < room ? n : room; if (k > 1400) k = 1400;
        if (tcp_write(conn, d, (u16_t)k, TCP_WRITE_FLAG_COPY) != ERR_OK) { tcp_output(conn); wup_sleep(1); continue; }
        d += k; n -= k; t = 0;
    }
    if (conn) tcp_output(conn);
}
static void puts_(const char *s) { out(s, (u32)strlen(s)); }
static void hex_(u32 v, int digits) { char b[8]; for (int i = 0; i < digits; i++) { int d = v >> (4 * (digits - 1 - i)) & 15; b[i] = d < 10 ? '0' + d : 'a' + d - 10; } out(b, (u32)digits); }

/* ---- input */
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) { tcp_arg(pcb, 0); tcp_recv(pcb, 0); tcp_close(pcb); if (pcb == conn) conn = 0; return ERR_OK; }
    for (struct pbuf *q = p; q; q = q->next) {
        const u8 *d = q->payload; u32 n = q->len;
        if (wb_active == 1) {                                  /* raw data of a wb: straight to the target */
            u32 k = n < wb_left ? n : wb_left;
            memcpy(wb_dst, d, k); wb_crc = crc32_update(wb_crc, d, k); wb_dst += k; wb_left -= k; d += k; n -= k;
            if (!wb_left) wb_active = 2;                       /* finish (sync + reply) in the step */
        }
        if (n) { u32 k = n < sizeof inq - inq_n ? n : sizeof inq - inq_n; memcpy(inq + inq_n, d, k); inq_n += k; }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static void on_err(void *arg, err_t err) { (void)arg; (void)err; conn = 0; wb_active = 0; }
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    if (conn) { tcp_abort(conn); conn = 0; }                    /* newest client wins */
    conn = pcb; inq_n = 0; wb_active = 0;
    sound_off();                                                /* (the host's tools use the audio ring's memory) */
    tcp_recv(pcb, on_recv); tcp_err(pcb, on_err);
    return ERR_OK;
}

/* ---- commands */
static char *tp;
static char *tok(void) { while (*tp == ' ') tp++; if (!*tp) return 0; char *s = tp; while (*tp && *tp != ' ') tp++; if (*tp) *tp++ = 0; return s; }
static int num(u32 *o) {
    char *s = tok(); if (!s) return 0; u32 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; if (!*s) return 0;
        for (; *s; s++) { int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1; if (d < 0) return 0; v = v << 4 | d; } }
    else for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (*s - '0'); }
    *o = v; return 1;
}
static int eq(const char *a, const char *b) { return !strcmp(a, b); }
static void ok(void) { puts_("OK\n"); }
static void err(const char *m) { puts_("ERR "); puts_(m); puts_("\n"); }
static void send_mem(const u8 *p, u32 n) {
    puts_("DATA "); hex_(n, 8); puts_("\n");
    u32 c = 0; u8 chunk[1024];
    for (u32 o = 0; o < n; o += sizeof chunk) {               /* checksum exactly the bytes sent */
        u32 k = n - o < sizeof chunk ? n - o : sizeof chunk;
        memcpy(chunk, p + o, k); c = crc32_update(c, chunk, k); out(chunk, k);
    }
    puts_("OK "); hex_(c, 8); puts_("\n");
}
static void run(char *line) {
    tp = line; char *c = tok(); if (!c) return;
    ncmds++;
    u32 a, b, v;
    if (eq(c, "ping")) { puts_("pong\n"); ok(); }
    else if (eq(c, "info")) { puts_("wupdbg 1 tcp max="); hex_(MAXLEN, 8); puts_("\n"); ok(); }
    else if (eq(c, "r8") || eq(c, "r16") || eq(c, "r32")) {
        int w = c[1] == '8' ? 1 : c[1] == '1' ? 2 : 4;
        if (!num(&a)) { err("usage: r32 ADDR [COUNT]"); return; }
        if (!num(&b)) b = 1;
        for (u32 i = 0; i < b; i++) {
            u32 p = a + i * w; v = w == 1 ? *(volatile u8 *)p : w == 2 ? *(volatile u16 *)p : *(volatile u32 *)p;
            hex_(v, w * 2); puts_(i % 8 == 7 || i == b - 1 ? "\n" : " ");
        }
        ok();
    }
    else if (eq(c, "w8") || eq(c, "w16") || eq(c, "w32")) {
        if (!num(&a) || !num(&v)) { err("usage: w32 ADDR VALUE"); return; }
        if (c[1] == '8') *(volatile u8 *)a = (u8)v; else if (c[1] == '1') *(volatile u16 *)a = (u16)v; else *(volatile u32 *)a = v;
        ok();
    }
    else if (eq(c, "rb")) { if (!num(&a) || !num(&b) || b > MAXLEN) { err("usage: rb ADDR LEN"); return; } send_mem((const u8 *)a, b); }
    else if (eq(c, "fr")) {
        if (!num(&a) || !num(&b) || b > MAXLEN) { err("usage: fr FLASHADDR LEN"); return; }
        puts_("DATA "); hex_(b, 8); puts_("\n");
        u32 crc = 0;
        for (u32 o = 0; o < b; o += 4096) {
            u32 k = b - o < 4096 ? b - o : 4096;
            if (WUPK->flash_read(a + o, FLBUF, k)) memset(FLBUF, 0, k);
            crc = crc32_update(crc, FLBUF, k); out(FLBUF, k);
        }
        puts_("OK "); hex_(crc, 8); puts_("\n");
    }
    else if (eq(c, "wb")) {
        if (!num(&a) || !num(&b) || !num(&wb_want) || b > MAXLEN) { err("usage: wb ADDR LEN CRC32"); return; }
        wb_dst = (u8 *)a; wb_left = wb_len = b; wb_crc = 0; wb_active = b ? 1 : 2;
        puts_("GO\n");
    }
    else if (eq(c, "fw")) {                                     /* one 4 KB flash sector: erased, written, read back */
        if (!num(&a) || !num(&b) || !num(&wb_want) || (a & 0xfff) || !b || b > 4096) { err("usage: fw SECTORADDR LEN CRC32"); return; }
        fw_addr = a; fw_on = 1;
        wb_dst = fw_buf; wb_left = wb_len = b; wb_crc = 0; wb_active = 1;
        puts_("GO\n");
    }
    else if (eq(c, "call")) {
        u32 r[4] = { 0, 0, 0, 0 };
        if (!num(&a)) { err("usage: call ADDR [R0 [R1 [R2 [R3]]]]"); return; }
        for (int i = 0; i < 4 && num(&r[i]); i++) ;
        unsigned long long res = ((unsigned long long (*)(u32, u32, u32, u32))a)(r[0], r[1], r[2], r[3]);
        puts_("OK "); hex_((u32)res, 8); puts_(" "); hex_((u32)(res >> 32), 8); puts_("\n");
    }
    else if (eq(c, "sleep")) { if (!num(&a)) a = 1; DLY((int)a); ok(); }
    else err("unknown command (ping info r8 r16 r32 w8 w16 w32 rb wb fr fw call sleep)");
}

void dbgsrv_step(void) {
    if (!conn) return;
    if (wb_active == 2) {                                       /* a wb (or fw) finished arriving */
        wb_active = 0;
        if (wb_crc != wb_want) { fw_on = 0; puts_("ERR crc "); hex_(wb_crc, 8); puts_("\n"); }
        else if (fw_on) {
            static u8 back[4096];
            fw_on = 0;
            if (WUPK->flash_erase4k(fw_addr)) err("erase failed");
            else if (WUPK->flash_write(fw_addr, fw_buf, wb_len)) err("write failed");
            else if (WUPK->flash_read(fw_addr, back, wb_len) || memcmp(back, fw_buf, wb_len)) err("read-back differs");
            else ok();
        }
        else { sync_code((u32)(wb_dst - wb_len), wb_len); ok(); }
    }
    if (wb_active) return;
    for (;;) {                                                  /* one complete line at a time */
        u32 i = 0; while (i < inq_n && inq[i] != '\n' && inq[i] != '\r') i++;
        if (i == inq_n) { if (inq_n == sizeof inq) inq_n = 0; return; }
        char line[200]; u32 k = i < sizeof line - 1 ? i : sizeof line - 1;
        memcpy(line, inq, k); line[k] = 0;
        memmove(inq, inq + i + 1, inq_n - i - 1); inq_n -= i + 1;
        run(line);
        if (wb_active) {                                        /* data that arrived with the command line */
            u32 n = inq_n < wb_left ? inq_n : wb_left;
            memcpy(wb_dst, inq, n); wb_crc = crc32_update(wb_crc, (const u8 *)inq, n); wb_dst += n; wb_left -= n;
            memmove(inq, inq + n, inq_n - n); inq_n -= n;
            if (!wb_left) wb_active = 2;
            return;
        }
    }
}
int dbgsrv_open(void) {
    if (listener) return 0;
    struct tcp_pcb *l = tcp_new();
    if (!l || tcp_bind(l, IP4_ADDR_ANY, 28802) != ERR_OK) { if (l) tcp_close(l); return -1; }
    listener = tcp_listen(l);
    if (!listener) return -2;
    tcp_accept(listener, on_accept);
    return 0;
}
void dbgsrv_close(void) {
    if (conn) { tcp_abort(conn); conn = 0; }
    if (listener) { tcp_close(listener); listener = 0; }
}
int dbgsrv_active(void) { return conn != 0; }
u32 dbgsrv_commands(void) { return ncmds; }
