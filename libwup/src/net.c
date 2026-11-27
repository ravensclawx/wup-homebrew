/* libwup networking.
 *
 * The Homebrew Menu owns Wi-Fi: it joins the network and stays associated when it starts an app, and hands the app
 * its address (the hand-off block net_*, include/handoff.h). wup_net_init() takes that over: the app's own lwIP
 * (src/wup_netif.c) on the same address, no DHCP, no Wi-Fi code in the app.
 *
 * net = yes: the sockets are lwIP's own (its "sockets" API), with the stack in two threads (lwIP's, and the radio's
 * in wup_netif.c); the standard names here just call them. Descriptors are 3..14; close/read/write on them come here
 * from src/sys.c. Any thread may use them. errno is one for all threads.
 * net = raw: no sockets: lwIP's callback API, polled from wup_sleep() (the menu's servers). */
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "wup_netif.h"
#include "wup.h"
#include "wupk.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
static int net_up, stack_up;

#if !NO_SYS
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#if !LWIP_IPV6
const struct in6_addr in6addr_any;                /* (IPv4 only; portable code names it) */
#endif
/* ---- the standard names */
int socket(int domain, int type, int protocol) { if (!stack_up) { errno = ENETDOWN; return -1; } return lwip_socket(domain, type, protocol); }
int bind(int fd, const struct sockaddr *addr, socklen_t len) { return lwip_bind(fd, addr, len); }
int listen(int fd, int backlog) { return lwip_listen(fd, backlog); }
int accept(int fd, struct sockaddr *addr, socklen_t *len) { return lwip_accept(fd, addr, len); }
int connect(int fd, const struct sockaddr *addr, socklen_t len) { return lwip_connect(fd, addr, len); }
ssize_t send(int fd, const void *buf, size_t n, int flags) { return lwip_send(fd, buf, n, flags); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return lwip_recv(fd, buf, n, flags); }
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t tolen) { return lwip_sendto(fd, buf, n, flags, to, tolen); }
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen) { return lwip_recvfrom(fd, buf, n, flags, from, fromlen); }
int shutdown(int fd, int how) { return lwip_shutdown(fd, how); }
int setsockopt(int fd, int level, int name, const void *val, socklen_t len) { return lwip_setsockopt(fd, level, name, val, len); }
int getsockopt(int fd, int level, int name, void *val, socklen_t *len) { return lwip_getsockopt(fd, level, name, val, len); }
int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return lwip_getsockname(fd, addr, len); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return lwip_getpeername(fd, addr, len); }
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) { return lwip_select(n, r, w, e, t); }
int poll(struct pollfd *fds, nfds_t n, int ms) { return lwip_poll(fds, n, ms); }
int ioctl(int fd, long cmd, void *arg) { return lwip_ioctl(fd, cmd, arg); }
int fcntl(int fd, int cmd, ...) { va_list ap; va_start(ap, cmd); int v = va_arg(ap, int); va_end(ap); return lwip_fcntl(fd, cmd, v); }
int inet_pton(int af, const char *s, void *dst) { return lwip_inet_pton(af, s, dst); }
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) { return lwip_inet_ntop(af, src, dst, size); }
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) { return lwip_getaddrinfo(node, service, hints, res); }
void freeaddrinfo(struct addrinfo *res) { lwip_freeaddrinfo(res); }
struct hostent *gethostbyname(const char *name) { return lwip_gethostbyname(name); }
const char *gai_strerror(int e) {
    return e == EAI_NONAME ? "name not found" : e == EAI_SERVICE ? "bad service" : e == EAI_FAIL ? "lookup failed" : e == EAI_MEMORY ? "out of memory" : e == EAI_FAMILY ? "IPv4 only" : "lookup error";
}
static int sock_close(int fd) { return lwip_close(fd); }
static ssize_t sock_read(int fd, void *buf, size_t n) { return lwip_read(fd, buf, n); }
static ssize_t sock_write(int fd, const void *buf, size_t n) { return lwip_write(fd, buf, n); }
extern ssize_t (*wup__sock_read)(int, void *, size_t), (*wup__sock_write)(int, const void *, size_t);
void wup__net_threads_kill(void);
static void up_done(void *arg) { wup_sem_signal((int)(intptr_t)arg); }
#define LOCK()   LOCK_TCPIP_CORE()
#define UNLOCK() UNLOCK_TCPIP_CORE()
#else
#define LOCK()
#define UNLOCK()
#endif

/* ---- debug log over Wi-Fi: wup_log() text, a datagram per line, broadcast to UDP port 28803. Whoever logs only
   leaves the text here; it is sent when the radio is next served (wup_netif.c, with lwIP's lock held). */
#define LOG_PORT 28803
static struct udp_pcb *log_pcb;
static char log_buf[1024]; static volatile int log_len;
static void log_send(void) {
    static char line[240];
    while (log_len && log_pcb) {
        unsigned st = wup_irq_off();
        int n = 0; while (n < log_len && n < (int)sizeof line && log_buf[n++] != '\n') { }
        memcpy(line, log_buf, (size_t)n); memmove(log_buf, log_buf + n, (size_t)(log_len - n)); log_len -= n;
        wup_irq_restore(st);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
        if (!p) return;
        memcpy(p->payload, line, (size_t)n); udp_sendto(log_pcb, p, IP_ADDR_BROADCAST, LOG_PORT); pbuf_free(p);
    }
}
static void net_log(const char *s) {
    unsigned st = wup_irq_off();
    for (; *s && log_len < (int)sizeof log_buf; s++) if (*s != '\r') log_buf[log_len++] = *s;
    wup_irq_restore(st);
}
static void log_start(void) {
    LOCK();
    log_pcb = udp_new();
    if (log_pcb) { log_pcb->so_options |= SOF_BROADCAST; wup__net_log = net_log; wup_netif_on_tick(log_send); }
    UNLOCK();
}

/* ---- bring-up */
static void net_exit(void) {
    wup__net_log = 0;
#if !NO_SYS
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN; fd++) lwip_close(fd);
    wup__sock_close = 0; wup__sock_read = 0; wup__sock_write = 0;
    wup_sleep(20);                               /* let FINs go out */
#else
    for (int i = 0; i < 20; i++) { wup_netif_poll(); wup_sleep(1); }
#endif
    wup_netif_on_tick(0);
    if (net_up) wup_netif_stop();
#if !NO_SYS
    LOCK();                                      /* (lwIP's thread is then not in the middle of anything) */
    wup__net_threads_kill();
#endif
    net_up = stack_up = 0;
}
static void stack(void) {                        /* lwIP itself; with sockets, 127.0.0.1 works from here on */
    if (stack_up) return;
    stack_up = 1;
    wup__net_exit = net_exit;
#if !NO_SYS
    int done = wup_sem_new(0, 1);
    tcpip_init(up_done, (void *)(intptr_t)done);
    wup_sem_wait(done, -1); wup_sem_free(done);
    wup__sock_close = sock_close; wup__sock_read = sock_read; wup__sock_write = sock_write;
#endif
}
static int start(const u8 *mac, u32 ip, u32 mask, u32 gw, u32 dns) {
    stack();
    if (wup_netif_start(mac, ip, mask, gw, dns)) return -2;
    net_up = 1;
    log_start();
#if NO_SYS
    wup__net_poll = wup_netif_poll;
#endif
    return 0;
}
int wup_net_init(void) {
    if (net_up) return 0;
    const wup_handoff_t *pb = wup_handoff();
#if !NO_SYS
    stack();
#endif
    if (!pb || !(pb->flags & WUP_HO_NET_INFO) || !pb->net_ip) return -1;
    return start((const u8 *)pb->net_mac, pb->net_ip, pb->net_mask, pb->net_gw, pb->net_dns);
}
/* the menu's side: the network stack on the radio before a network is joined, then DHCP; the address it gets is
   what apps take over (wup_net_info -> the hand-off block) */
int wup_net_start(const uint8_t mac[6]) { return net_up ? 0 : start(mac, 0, 0, 0, 0); }
int wup_net_dhcp(void) { return wup_netif_dhcp(); }
void wup_net_leave(void) { wup_netif_leave(); }
void wup_net_info(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns) {
    LOCK();
    const ip_addr_t *d = dns_getserver(0);
    wup_netif_netinfo(ip, mask, gw); *dns = d ? ip4_addr_get_u32(d) : 0;
    UNLOCK();
}
int wup_net_online(void) { return net_up; }
uint32_t wup_net_ip(void) { return net_up ? wup_netif_ip() : 0; }
int wup_net_tx_room(void) { return WUPK->wifi_tx_room(); }
const char *wup_net_ip_str(void) { static char b[16]; ip4_addr_t a; ip4_addr_set_u32(&a, wup_net_ip()); return ip4addr_ntoa_r(&a, b, sizeof b); }

/* counters for diagnosing a busy link: frames, drops, pool and heap exhaustion, retransmissions */
#include "lwip/stats.h"
int wup_net_stats(char *o, int n) {
    const wup_netif_stats_t *p = wup_netif_stats();
    u32 v[] = { p->rx_frames, p->rx_dropped, p->tx_frames, p->tx_errors, p->tx_queue_max, p->tx_queue_full, lwip_stats.memp[MEMP_PBUF_POOL]->err,
                lwip_stats.memp[MEMP_PBUF_POOL]->max, lwip_stats.mem.err, lwip_stats.mem.max, lwip_stats.tcp.memerr, lwip_stats.tcp.drop };
    static const char *k[] = { "rx", "rx_dropped", "tx", "tx_errors", "txq_max", "txq_full", "pool_err", "pool_max", "heap_err", "heap_max", "tcp_memerr", "tcp_drop" };
    int len = 0;
    for (int i = 0; i < 12 && len < n - 24; i++) {
        const char *s = k[i]; while (*s) o[len++] = *s++;
        o[len++] = '=';
        char t[12]; int m = 0; u32 x = v[i]; do t[m++] = (char)('0' + x % 10); while (x /= 10);
        while (m) o[len++] = t[--m];
        o[len++] = ' ';
    }
    o[len] = 0;
    return len;
}

void wup_net_tx_pacing(unsigned r, unsigned b) { wup_netif_tx_pacing(r, b); }

/* the date and time from the network (lwIP's SNTP client), for the menu and any app with net = yes: started once,
   asked again every hour. Apps also get the time from the menu in the hand-off block; this covers an app started
   before the menu had heard back (the App Store then could not check any certificate) */
#include "lwip/apps/sntp.h"
void wup__sntp_time(unsigned sec, unsigned us) {
    wup_time_set((int64_t)sec, us);
    wup_log("[net] the time from the network\n");
}
void wup_net_time(void) {
    static int on;
    if (on || !net_up) return;
    LOCK();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_setservername(2, "time.cloudflare.com");
    sntp_init();
    UNLOCK();
    on = 1;
}

/* lwIP asserts: logged on the UART */
void wup_net_assert(const char *msg, const char *file, int line) {
    (void)file; wup_log("[net] lwIP assert: "); wup_log(msg); wup_log(" line "); wup_log_hex((u32)line); wup_log("\n");
}
