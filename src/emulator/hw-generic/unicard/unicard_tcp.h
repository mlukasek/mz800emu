/*
 * unicard_tcp.h - the TCP client and DNS of the Unicard (firmware commands TCPSTATUS 80h .. TCPRECV 86h,
 * DNSREQ 88h, DNSRES 89h) on the host's sockets.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 */

#ifndef UNICARD_TCP_H
#define UNICARD_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* TCPSTATUS byte 1 (firmware 0.33): the state of the connection */
#define UNICARD_TCP_NONE        0   /* no connection */
#define UNICARD_TCP_CONNECTING  1
#define UNICARD_TCP_CONNECTED   2
#define UNICARD_TCP_FAILED      3   /* refused, reset, unreachable (as the DPI-1 firmware) */
#define UNICARD_TCP_CLOSED      4   /* the server closed it, its data still readable */

#define UNICARD_TCP_BUF         3072 /* the received data the card keeps (>= the TCP window of the mk3b) */

/* DNSRES byte 0 (emu_DNS.h of the firmware) */
#define UNICARD_DNS_IDLE        0
#define UNICARD_DNS_RESOLVING   1
#define UNICARD_DNS_RESOLVED    2
#define UNICARD_DNS_UNKNOWN     3
#define UNICARD_DNS_BAD_ARGS    4

extern void unicard_tcp_set_ip ( const uint8_t *ip );
extern void unicard_tcp_set_port ( uint16_t port );
extern void unicard_tcp_open ( void );
extern void unicard_tcp_close ( void );
extern void unicard_tcp_status ( uint8_t *out );    /* 10 B: connected, state, WORD received, IP, WORD port */
extern int unicard_tcp_recv ( uint8_t *dst, int max );
extern void unicard_tcp_send ( const uint8_t *data, int len );
extern void unicard_dns_request ( const char *name );
extern void unicard_dns_result ( uint8_t *out );    /* 5 B: status, IP */

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_TCP_H */
