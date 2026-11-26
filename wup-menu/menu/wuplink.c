/* wuplink: receive an app (.wup) over the network for the menu to keep and start. lwIP's raw API, driven by
 * the menu's loop.
 *
 * Protocol v1 (tools/wuplink.py is the sender):
 *   discovery  UDP 28801: "wupboot" -> the pad answers "bootwup" (so the sender finds it)
 *   transfer   TCP 28800: sender -> "WUPL" u32 version (1; flags << 16 are ignored) u32 size
 *                         u32 crc32 u32 namelen, name
 *                         pad    -> u32 status (0 = go ahead, 1 = too big, 2 = bad header, 3 = busy, 5 = no room)
 *                         sender -> size bytes of the app image
 *                         pad    -> u32 status (0 = received and CRC ok, 4 = CRC mismatch, 5 = no room)
 * The .wup is written straight to WUPLINK_STREAM_PATH in flash as it arrives; the menu then moves it into /apps
 * and starts it. Little endian throughout. */
#include <stdint.h>
#include <string.h>
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "wuplink.h"
typedef uint8_t u8; typedef uint32_t u32;

#define TCP_PORT 28800
#define UDP_PORT 28801
#define STREAM_MAX 0x00100000u
#include "fs.h"
#include "armcpu.h"

static wup_status_t st;
static struct tcp_pcb *listener, *conn;
static struct udp_pcb *disc;
static u8 hdr[20 + 64]; static u32 hdr_got, hdr_need = 20;
static u32 want_crc;

/* room for an app of n bytes, beside what is stored (littlefs keeps a little for its own records: 16 KB spare) */
static int room_for(u32 n) { u32 size = fs_size_kb(), used = fs_used_kb(); return used + n / 1024 + 1 + 16 <= size; }
static void reply(u32 v) { if (conn) { tcp_write(conn, &v, 4, TCP_WRITE_FLAG_COPY); tcp_output(conn); } }
static void drop_conn(void) {
    if (!conn) return;
    tcp_arg(conn, 0); tcp_recv(conn, 0); tcp_err(conn, 0);
    tcp_close(conn); conn = 0;
}
/* the sender is gone: in the middle of an app that is an error; before one began (a port check, a cancelled send) the
   Netloader just waits again */
static void gone(const char *why) {
    if (st.state == WUP_RECEIVING) { fs_stream_close(); st.state = WUP_ERROR; st.error = why; }
    else if (st.state == WUP_HEADER) st.state = WUP_WAITING;
}
static void conn_err(void *arg, err_t err) { (void)arg; (void)err; conn = 0; gone("Connection lost"); }

/* what arrives waits here, unacknowledged (TCP holds the sender back), until wuplink_step writes it: lwIP's callbacks
   run in its own thread, and the filesystem is the menu thread's */
static struct pbuf *pend; static int fin;
static err_t conn_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err; (void)pcb;
    if (!p) { fin = 1; return ERR_OK; }
    if (pend) pbuf_cat(pend, p); else pend = p;
    return ERR_OK;
}
static void take(struct pbuf *p) {                     /* (the menu's thread, under lwIP's lock: wuplink_step) */
    for (struct pbuf *q = p; q; q = q->next) {
        const u8 *d = q->payload; u32 n = q->len;
        while (n) {
            if (st.state == WUP_HEADER) {
                u32 k = hdr_need - hdr_got; if (k > n) k = n;
                memcpy(hdr + hdr_got, d, k); hdr_got += k; d += k; n -= k;
                if (hdr_got == 20 && hdr_need == 20) {
                    u32 magic, ver, namelen;
                    memcpy(&magic, hdr, 4); memcpy(&ver, hdr + 4, 4); memcpy(&st.total, hdr + 8, 4); memcpy(&want_crc, hdr + 12, 4); memcpy(&namelen, hdr + 16, 4);
                    if (magic != 0x4c505557u || (ver & 0xffff) != 1 || namelen > 63) { reply(2); st.state = WUP_ERROR; st.error = "The computer did not send an app"; break; }
                    hdr_need = 20 + namelen;
                }
                if (hdr_got == hdr_need && hdr_need >= 20) {
                    u32 namelen = hdr_need - 20;
                    memcpy(st.name, hdr + 20, namelen); st.name[namelen] = 0;
                    if (!st.total || st.total > STREAM_MAX) { reply(1); st.state = WUP_ERROR; st.error = "The app is too large (1 MB at most)"; break; }
                    st.to_file = 1;
                    if (fs_ok()) fs_remove(WUPLINK_STREAM_PATH);   /* (one left from a transfer that broke off) */
                    if (fs_ok() && !room_for(st.total)) { reply(5); st.state = WUP_ERROR; st.error = "Not enough storage on the GamePad"; break; }
                    if (st.to_file && (!fs_ok() || fs_stream_open(WUPLINK_STREAM_PATH))) { reply(1); st.state = WUP_ERROR; st.error = "Storage is not available"; break; }
                    st.done = 0; st.crc = 0; st.state = WUP_RECEIVING;
                    reply(0);
                }
            } else if (st.state == WUP_RECEIVING) {
                u32 k = st.total - st.done; if (k > n) k = n;
                if (st.to_file) {
                    if (fs_stream_write(d, k)) { fs_stream_close(); fs_remove(WUPLINK_STREAM_PATH); reply(5); st.state = WUP_ERROR; st.error = "Not enough storage on the GamePad"; break; }
                }
                st.crc = crc32_update(st.crc, d, k);
                st.done += k; d += k; n -= k;
                if (st.done == st.total) {
                    if (st.to_file && fs_stream_close()) { fs_remove(WUPLINK_STREAM_PATH); reply(5); st.state = WUP_ERROR; st.error = "Not enough storage on the GamePad"; }
                    else if (st.crc == want_crc) { reply(0); st.state = WUP_READY; }
                    else { fs_remove(WUPLINK_STREAM_PATH); reply(4); st.state = WUP_ERROR; st.error = "The transfer was damaged"; }
                }
            } else n = 0;                                   /* ignore anything after the end */
        }
    }
}
static void wuplink_step_(void) {
    if (pend) {
        struct pbuf *p = pend; pend = 0;
        take(p);
        if (conn) tcp_recved(conn, p->tot_len);
        pbuf_free(p);
    }
    if (fin) { fin = 0; drop_conn(); gone("Sender disconnected"); }
}
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    if (conn || st.state != WUP_WAITING) { u32 v = 3; tcp_write(pcb, &v, 4, TCP_WRITE_FLAG_COPY); tcp_output(pcb); tcp_close(pcb); return ERR_OK; }
    conn = pcb; hdr_got = 0; hdr_need = 20; fin = 0;
    if (pend) { pbuf_free(pend); pend = 0; }
    st.state = WUP_HEADER; st.name[0] = 0; st.done = st.total = 0; st.to_file = 0;
    tcp_recv(pcb, conn_recv); tcp_err(pcb, conn_err);
    return ERR_OK;
}
static void disc_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    char b[8] = {0};
    if (pbuf_copy_partial(p, b, 7, 0) == 7 && !memcmp(b, "wupboot", 7)) {
        struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, 7, PBUF_RAM);
        if (r) { memcpy(r->payload, "bootwup", 7); udp_sendto(pcb, r, addr, port); pbuf_free(r); }
    }
    pbuf_free(p);
}

static int wuplink_open_(void) {
    if (listener) return 0;
    memset(&st, 0, sizeof st);
    struct tcp_pcb *l = tcp_new();
    if (!l || tcp_bind(l, IP4_ADDR_ANY, TCP_PORT) != ERR_OK) { if (l) tcp_close(l); return -1; }
    listener = tcp_listen(l);
    if (!listener) return -2;
    tcp_accept(listener, on_accept);
    disc = udp_new();
    if (disc && udp_bind(disc, IP4_ADDR_ANY, UDP_PORT) == ERR_OK) udp_recv(disc, disc_recv, 0);
    st.state = WUP_WAITING;
    return 0;
}
static void wuplink_close_(void) {
    drop_conn(); fin = 0;
    if (pend) { pbuf_free(pend); pend = 0; }
    if (listener) { tcp_close(listener); listener = 0; }
    if (disc) { udp_remove(disc); disc = 0; }
    st.state = WUP_OFF;
}
static void wuplink_rearm_(void) { drop_conn(); st.state = listener ? WUP_WAITING : WUP_OFF; st.error = 0; }
const wup_status_t *wuplink_status(void) { return &st; }
int wuplink_port(void) { return TCP_PORT; }

/* lwIP runs in its own thread (the menu: net = yes): its callbacks above run there, holding its core lock; these run
   on the menu's thread under the same lock, so the two never overlap */
#include "lwip/tcpip.h"
int wuplink_open(void) { LOCK_TCPIP_CORE(); int r = wuplink_open_(); UNLOCK_TCPIP_CORE(); return r; }
void wuplink_close(void) { LOCK_TCPIP_CORE(); wuplink_close_(); UNLOCK_TCPIP_CORE(); }
void wuplink_rearm(void) { LOCK_TCPIP_CORE(); wuplink_rearm_(); UNLOCK_TCPIP_CORE(); }
void wuplink_step(void) { LOCK_TCPIP_CORE(); wuplink_step_(); UNLOCK_TCPIP_CORE(); }
