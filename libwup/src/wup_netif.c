/* wup_netif: the GamePad's Wi-Fi as an lwIP network interface for apps (libwup's networking, src/net.c).
 *
 * The radio is the kernel's and stays joined from app to app; the Homebrew Menu hands over the address it has (the
 * hand-off block net_*). Received frames come from the kernel's radio task through a hook (wupk.h wifi_hooks): it
 * only copies each into a byte ring, and wup_netif_poll() hands the ring to lwIP from the app's own loop or thread.
 * Frames go out through the kernel (wifi_tx). When the app's networking stops, the hook is taken away again: frames
 * must never reach an app that is gone. */
#include <stdint.h>
#include <string.h>
#include "armcpu.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "lwip/tcpip.h"
#if NO_SYS
#define LOCK()
#define UNLOCK()
#else                                                /* lwIP in its own threads: we call it under its lock */
#include "wup.h"
#define LOCK()   LOCK_TCPIP_CORE()
#define UNLOCK() UNLOCK_TCPIP_CORE()
#endif
#include "wup_netif.h"
#include "wupk.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

#define HWCLK (*(volatile u32 *)0xf0000408u)        /* ~997.3 kHz, measured on the pad */

#ifndef WUP_NETIF_RING
#define WUP_NETIF_RING (64 * 1024)                       /* received frames waiting for wup_netif_poll */
#endif
/* byte ring: records of u32 length + frame, 4-byte aligned; length 0xffffffff = continue at the start */
static u8 ring[WUP_NETIF_RING] __attribute__((aligned(32), section(".bss.net")));
static volatile u32 r_wr, r_rd;                      /* offsets: written by k_rx / wup_netif_poll */
static wup_netif_stats_t st;

/* ---- clock and randomness for lwIP */
static u32 clock_ms, clk_last, clk_frac, rng = 0x9e3779b9;
#if NO_SYS
u32_t sys_now(void) {
    u32 now = HWCLK; clk_frac += now - clk_last; clk_last = now;
    clock_ms += clk_frac / 997; clk_frac %= 997;
    return clock_ms;
}
#endif
unsigned wup_netif_rand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng ^ HWCLK; }


/* ---- receive: the radio driver's task -> ring */
static void ring_put(const u8 *data, u32 len) {
    u32 need = (4 + len + 3) & ~3u, wr = r_wr, rd = r_rd;
    if (data && len && len <= 1536) {
        if (wr + need + 4 > WUP_NETIF_RING) {                 /* no room before the end: wrap (if the reader left room) */
            if (rd > wr || rd <= need) { st.rx_dropped++; goto out; }
            *(u32 *)(ring + wr) = 0xffffffffu; wr = 0;
        }
        u32 free_ = rd > wr ? rd - wr : WUP_NETIF_RING - wr + rd;
        if (need + 4 >= free_) { st.rx_dropped++; goto out; }
        *(u32 *)(ring + wr) = len;
        memcpy(ring + wr + 4, data, len);
        __asm__ volatile("" ::: "memory");          /* the record before the index that publishes it */
        r_wr = wr + need;
    } else st.rx_dropped++;
out:;
}
/* the wup-homebrew kernel's radio task hands each frame here (wupk.h wifi_hooks) */
static void k_rx(const u8 *eth, u32 len) { ring_put(eth, len); }

/* ---- transmit. The radio loses frames handed to it back to back (measured: of 50 echoes sent at once
   about a third never arrive, every one does 2 ms apart), and dhd_sendpkt still reports success. So
   frames are paced by a token bucket (TX_BURST at once, then TX_RATE per ms); the rest wait in TXQ,
   drained by wup_netif_poll() and by the next send. */
#ifndef WUP_NETIF_TX_RATE_X10
#define WUP_NETIF_TX_RATE_X10 15                         /* frames per ms, times 10 */
#endif
#ifndef WUP_NETIF_TX_BURST
#define WUP_NETIF_TX_BURST 8
#endif
#define TXQ 64
static struct pbuf *txq[TXQ];
static u32 txq_h, txq_n, tok_x10 = WUP_NETIF_TX_BURST * 10, tok_t, rate_x10 = WUP_NETIF_TX_RATE_X10, burst = WUP_NETIF_TX_BURST;
void wup_netif_tx_pacing(unsigned r_x10, unsigned b) { if (r_x10) rate_x10 = r_x10; if (b) burst = b; }

/* Nothing is sent before the radio has joined a network (a frame handed to it then made the radio chip fault). */
static int link_up;
static int radio_send(struct pbuf *p) {
    u32 len = p->tot_len;
    if (!link_up) return 0;                          /* (dropped) */
    if (len < 14 || len > 1514) return -1;
    static u8 f[1516] __attribute__((aligned(4)));
    pbuf_copy_partial(p, f, (u16_t)len, 0);
    if (WUPK->wifi_tx(f, len)) { st.tx_errors++; return -1; }
    st.tx_frames++;
    return 0;
}
static void tokens(void) {
    u32 now = HWCLK, us = now - tok_t;
    if (us < 100) return;
    tok_t = now;
    u32 add = us * rate_x10 / 1000;                  /* tokens x10 */
    tok_x10 = tok_x10 + add > burst * 10 ? burst * 10 : tok_x10 + add;
}
static void tx_drain(void) {
    tokens();
    while (txq_n && tok_x10 >= 10) {
        struct pbuf *p = txq[txq_h];
        txq_h = (txq_h + 1) % TXQ; txq_n--;
        radio_send(p); pbuf_free(p);
        tok_x10 -= 10;
    }
}
static err_t linkoutput(struct netif *nif, struct pbuf *p) {
    (void)nif;
    tx_drain();
    if (!txq_n && tok_x10 >= 10) { tok_x10 -= 10; return radio_send(p) ? ERR_IF : ERR_OK; }
    if (txq_n >= TXQ) { st.tx_queue_full++; return ERR_MEM; }
    pbuf_ref(p);                                     /* lwIP may free its reference when we return */
    txq[(txq_h + txq_n) % TXQ] = p; txq_n++;
    if (txq_n > st.tx_queue_max) st.tx_queue_max = txq_n;
    return ERR_OK;
}

static struct netif nif;
static u8 hwaddr[6];
static err_t nif_init(struct netif *n) {
    n->name[0] = 'w'; n->name[1] = 'l';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, hwaddr, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* ---- the receive hook: the kernel's radio task hands each frame to k_rx (into the ring) */
static void hook(int on) { WUPK->wifi_hooks(on ? k_rx : 0, 0); }

static int up;
static void pump(void);
static void (*tick)(void);
void wup_netif_on_tick(void (*fn)(void)) { tick = fn; }
#if !NO_SYS
/* the radio's thread: received frames into lwIP and waiting ones out, every millisecond, under lwIP's lock
   (lwIP's own thread runs its timers) */
static int rx_thread = -1; static volatile int rx_run;
static void rx_loop(void *arg) {
    (void)arg;
    while (rx_run) { LOCK(); pump(); UNLOCK(); wup_thread_sleep(1); }
}
#endif
int wup_netif_start(const u8 *mac, u32 ip, u32 mask, u32 gw, u32 dns) {
    if (up) return 0;
    memcpy(hwaddr, mac, 6);
    rng ^= (u32)mac[5] << 24 | (u32)mac[4] << 16 | HWCLK;
    clk_last = HWCLK; tok_t = HWCLK;
    r_wr = r_rd = 0;
#if NO_SYS
    lwip_init();
#endif                                               /* (else the caller has started lwIP: tcpip_init) */
    ip4_addr_t a, m, g;
    ip4_addr_set_u32(&a, ip); ip4_addr_set_u32(&m, mask); ip4_addr_set_u32(&g, gw);
    LOCK();
    if (!netif_add(&nif, &a, &m, &g, 0, nif_init, ethernet_input)) { UNLOCK(); return -1; }
    netif_set_default(&nif);
    netif_set_up(&nif);
    if (dns) { ip_addr_t d; ip_addr_set_ip4_u32(&d, dns); dns_setserver(0, &d); }
    hook(1);                                             /* frames to us from now on */
    up = 1;
    link_up = ip != 0;                               /* an address handed over: the network is joined */
    if (ip) etharp_gratuitous(&nif);                         /* tell the network where we are (new stack, same address) */
    UNLOCK();
#if !NO_SYS
    rx_run = 1; rx_thread = wup_thread_start(rx_loop, 0, 4096, 1);
    if (rx_thread < 0) return -4;
#endif
    return 0;
}

#if NO_SYS
void wup_netif_poll(void) { if (up) { pump(); sys_check_timeouts(); } }
#else
void wup_netif_poll(void) { }
#endif
static void pump(void) {
    if (!up) return;
    u32 wr = r_wr, rd = r_rd;
    __asm__ volatile("" ::: "memory");
    while (rd != wr) {
        u32 len = *(u32 *)(ring + rd);
        if (len == 0xffffffffu) { rd = 0; continue; }
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p) {
            pbuf_take(p, ring + rd + 4, (u16_t)len);
            st.rx_frames++;
            if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
        } else st.rx_dropped++;
        rd += (4 + len + 3) & ~3u;
        __asm__ volatile("" ::: "memory");
        r_rd = rd;                                   /* the driver may use the space now */
        wr = r_wr;
        __asm__ volatile("" ::: "memory");
    }
    tx_drain();
    if (tick) tick();
}

void wup_netif_stop(void) {
    if (!up) return;
#if !NO_SYS
    rx_run = 0; if (rx_thread >= 0 && wup_thread_join(rx_thread, 500)) wup_thread_kill(rx_thread);
    rx_thread = -1;
#endif
    LOCK();
    for (u32 t0 = HWCLK; txq_n && HWCLK - t0 < 50000; ) tx_drain();      /* up to 50 ms for the last frames */
    while (txq_n) { pbuf_free(txq[txq_h]); txq_h = (txq_h + 1) % TXQ; txq_n--; }
    hook(0);
    link_up = 0;
    up = 0;
    UNLOCK();
}

int wup_netif_dhcp(void) { if (!up) return -1; LOCK(); link_up = 1; int r = (int)dhcp_start(&nif); UNLOCK(); return r; }      /* (the menu: after joining a network) */
void wup_netif_leave(void) {                        /* (the menu: the network is forgotten) the address given back, none held */
    if (!up) return;
    LOCK(); dhcp_release_and_stop(&nif); netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4); link_up = 0; UNLOCK();
}
void wup_netif_netinfo(u32 *ip, u32 *mask, u32 *gw) {
    *ip = ip4_addr_get_u32(netif_ip4_addr(&nif)); *mask = ip4_addr_get_u32(netif_ip4_netmask(&nif)); *gw = ip4_addr_get_u32(netif_ip4_gw(&nif));
}
int wup_netif_up(void) { return up; }
u32 wup_netif_ip(void) { return up ? ip4_addr_get_u32(netif_ip4_addr(&nif)) : 0; }
const wup_netif_stats_t *wup_netif_stats(void) { return &st; }
