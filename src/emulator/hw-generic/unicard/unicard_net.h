/*
 * unicard_net.h - NET A0h-A9h of the Unicard (multiplayer through a relay) on the host's sockets.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * As the Unicard mk3b firmware 0.30-0.38 (uc3 v0.37) and the MZPico NET protocol (BomberNet docs/net-protocol.md):
 * rooms of one game, an input broadcast for lockstep games, hashes and messages. NETCREATE and NETJOIN wait for
 * the relay (status bit 6), the rest is answered at once from the state the relay's lines keep up to date.
 * The relay: the WebSocket ws://api.mzpico.com/net by default, or JSON lines over TCP (BomberNet relay/relay.py,
 * port 8766), set as on the card in /unicard/network.cfg (NETRELAY, NETRELAYADDR, NETRELAYPORT); [UNICARD]
 * net_relay = host[:port] (as the MZPico fork of the emulator) chooses JSON lines over TCP and takes precedence.
 */

#ifndef UNICARD_NET_H
#define UNICARD_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define UNICARD_NET_INFO_FEATURE 0x08   /* INFO 95h byte 3: NET available */
#define UNICARD_NET_OUT_MAX 34          /* the longest output (NETRECV) */

    /* error codes (status byte 2) */
#define UNICARD_NET_ERR_BUILD 6         /* another build of the game */
#define UNICARD_NET_ERR_ROOM 7          /* the room does not exist or is full */
#define UNICARD_NET_ERR_NOROOM 8        /* not in a room */
#define UNICARD_NET_ERR_NOLINK 9        /* no connection with the relay */
#define UNICARD_NET_ERR_PARAM 10        /* a bad parameter */
#define UNICARD_NET_ERR_FULL 11         /* a buffer is full */

    extern int unicard_net_is_cmd ( uint8_t cmd );
    /* parameters of the command for unimgr_input_params ("" = none) */
    extern const char* unicard_net_param_format ( uint8_t cmd );
    /* Runs the command: 0 = done (out, out_len), an error code, or -1 = waits for the relay (unicard_net_async_poll). */
    extern int unicard_net_exec ( uint8_t cmd, const uint8_t *p, uint8_t *out, int *out_len );
    /* 0 = still waiting, 1 = done (out, out_len), > 1 = an error code */
    extern int unicard_net_async_poll ( uint8_t *out, int *out_len );
    extern void unicard_net_cancel ( void );    /* a new command cancels a waiting NETCREATE / NETJOIN */
    extern void unicard_net_reset ( void );     /* a reset of the computer: leave the room, close the connection */
    extern void unicard_net_pump ( void );      /* the socket: connect, send, receive; the 15 s wait for the relay */

    /* Tests: no sockets. The relay "connects" at once, the bytes sent are collected, received bytes are given. */
    extern void unicard_net_test_mode ( int on );
    extern void unicard_net_test_recv ( const uint8_t *data, int len );
    extern int unicard_net_test_sent ( uint8_t *buf, int max );    /* the bytes sent since the last call */
    extern void unicard_net_test_advance ( int ms );                /* moves the clock of the waiting */

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_NET_H */
