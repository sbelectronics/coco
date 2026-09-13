#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
// 16 KB: tcp_write(COPY) of a full ROM-list response (~11 KB of sndbuf)
// allocates from this pool; 8 KB would make large responses crawl
// through repeated ERR_MEM retries.
#define MEM_SIZE                    16000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// --- multicart additions ---------------------------------------------
// ROM uploads are long single-direction streams; a bigger receive
// window keeps them moving while the core-0 pump drains pbufs into
// flash between OLED refreshes.
#undef  PBUF_POOL_SIZE
#undef  TCP_WND
#if MC_BOARD_VIDEOCART
// The videocart has no spare SRAM: 96 KB of permuted serve tables and
// 125 KB of HDMI framebuffer leave the pbuf pool as the only place to
// find room, and the serve tables are load-bearing while a big receive
// window is merely faster. 14 buffers x ~1.5 KB still keeps uploads
// streaming; they simply refill more often.
#define PBUF_POOL_SIZE              14
#define TCP_WND                     (6 * TCP_MSS)
#else
// ROM uploads are long single-direction streams; a bigger receive window
// keeps them moving while the core-0 pump drains pbufs into flash.
#define PBUF_POOL_SIZE              32
#define TCP_WND                     (12 * TCP_MSS)
#endif
#define MEMP_NUM_TCP_PCB            8

// mDNS so the cart answers at coco-multicart.local.
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define LWIP_IGMP                   1
#define MDNS_MAX_SERVICES           1

// mDNS schedules sys_timeouts (startup probe/announce + one per delayed
// query response) that lwIP does NOT include in its internal default
// (LWIP_NUM_SYS_TIMEOUT_INTERNAL). With TCP+ARP+2*DHCP+IGMP+DNS the
// default pool is already fully accounted for, so the first mDNS timer
// exhausts it -> runtime panic "sys_timeout: ... MEMP_SYS_TIMEOUT is
// empty" the moment WiFi + mDNS + web come up. Track the internal count
// and add headroom for mDNS.
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define API_LIB_DEBUG               LWIP_DBG_OFF
#define API_MSG_DEBUG               LWIP_DBG_OFF
#define SOCKETS_DEBUG               LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_OFF
#define INET_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define IP_REASS_DEBUG              LWIP_DBG_OFF
#define RAW_DEBUG                   LWIP_DBG_OFF
#define MEM_DEBUG                   LWIP_DBG_OFF
#define MEMP_DEBUG                  LWIP_DBG_OFF
#define SYS_DEBUG                   LWIP_DBG_OFF
#define TCP_DEBUG                   LWIP_DBG_OFF
#define TCP_INPUT_DEBUG             LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG            LWIP_DBG_OFF
#define TCPIP_DEBUG                 LWIP_DBG_OFF
#define PPP_DEBUG                   LWIP_DBG_OFF
#define SLIP_DEBUG                  LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF

#endif
