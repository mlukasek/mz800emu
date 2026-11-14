/*
 * unicard_tcp.c - the TCP client and DNS of the Unicard on the host's sockets.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * The firmware mk3b (emu_TCP.c, emu_DNS.c) has one connection, client only. As the firmware 0.33: the received
 * data wait in a buffer of UNICARD_TCP_BUF bytes until the Z80 reads them (TCPRECV, at most 253 B a time); what does
 * not fit stays in the host's socket (the firmware: the TCP window, the server waits). TCPSTATUS byte 1 is the
 * state of the connection (UNICARD_TCP_*), byte 0 is 1 only while connected. A connection the server closed keeps
 * its received data readable. DNS asks the host's resolver at once (DNSRES is then RESOLVED or UNKNOWN).
 * Non-blocking sockets: the emulation never waits for the network. TCP_NODELAY as the firmware 0.33 (no Nagle:
 * every key goes at once).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "unicard_tcp.h"

static struct {
    SOCKET sock;
    uint8_t state;
    uint8_t ip[4];
    uint16_t port;
    uint8_t rx[UNICARD_TCP_BUF];
    int rx_len;
    uint8_t dns_state;
    uint8_t dns_ip[4];
} s_tcp = { INVALID_SOCKET, UNICARD_TCP_NONE };


static int tcp_net_init ( void ) {
#ifdef _WIN32
    static int done = 0;
    if ( !done ) {
        WSADATA wsa;
        if ( WSAStartup ( MAKEWORD ( 2, 2 ), &wsa ) ) return 0;
        done = 1;
    };
#endif
    return 1;
}


static int tcp_would_block ( void ) {
#ifdef _WIN32
    int e = WSAGetLastError ( );
    return ( e == WSAEWOULDBLOCK ) || ( e == WSAEINPROGRESS );
#else
    return ( errno == EWOULDBLOCK ) || ( errno == EAGAIN ) || ( errno == EINPROGRESS );
#endif
}


static void tcp_drop ( uint8_t state ) {
    if ( s_tcp.sock != INVALID_SOCKET ) closesocket ( s_tcp.sock );
    s_tcp.sock = INVALID_SOCKET;
    s_tcp.state = state;
}


/* tcp_pump: the connect finished? the received data into the buffer, as much as fits */
static void tcp_pump ( void ) {
    if ( s_tcp.sock == INVALID_SOCKET ) return;
    if ( s_tcp.state == UNICARD_TCP_CONNECTING ) {
        fd_set wr, ex;
        struct timeval tv = { 0, 0 };
        FD_ZERO ( &wr );
        FD_ZERO ( &ex );
        FD_SET ( s_tcp.sock, &wr );
        FD_SET ( s_tcp.sock, &ex );
        if ( select ( (int) s_tcp.sock + 1, NULL, &wr, &ex, &tv ) <= 0 ) return;
        int err = 0;
        socklen_t len = sizeof ( err );
        if ( FD_ISSET ( s_tcp.sock, &ex )
             || getsockopt ( s_tcp.sock, SOL_SOCKET, SO_ERROR, (char *) &err, &len ) || err ) {
            tcp_drop ( UNICARD_TCP_FAILED );
            return;
        };
        s_tcp.state = UNICARD_TCP_CONNECTED;
    };
    while ( s_tcp.state == UNICARD_TCP_CONNECTED && s_tcp.rx_len < UNICARD_TCP_BUF ) {
        int n = recv ( s_tcp.sock, (char *) &s_tcp.rx[s_tcp.rx_len], UNICARD_TCP_BUF - s_tcp.rx_len, 0 );
        if ( n > 0 ) {
            s_tcp.rx_len += n;
        } else if ( n == 0 ) {
            tcp_drop ( UNICARD_TCP_CLOSED );
        } else {
            if ( !tcp_would_block ( ) ) tcp_drop ( UNICARD_TCP_FAILED );
            return;
        };
    };
}


void unicard_tcp_set_ip ( const uint8_t *ip ) {
    memcpy ( s_tcp.ip, ip, 4 );
}


void unicard_tcp_set_port ( uint16_t port ) {
    s_tcp.port = port;
}


void unicard_tcp_open ( void ) {
    tcp_drop ( UNICARD_TCP_NONE );
    s_tcp.rx_len = 0;
    if ( !tcp_net_init ( ) ) {
        s_tcp.state = UNICARD_TCP_FAILED;
        return;
    };
    s_tcp.sock = socket ( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( s_tcp.sock == INVALID_SOCKET ) {
        s_tcp.state = UNICARD_TCP_FAILED;
        return;
    };
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket ( s_tcp.sock, FIONBIO, &nb );
#else
    fcntl ( s_tcp.sock, F_SETFL, fcntl ( s_tcp.sock, F_GETFL, 0 ) | O_NONBLOCK );
#endif
    int nodelay = 1;
    setsockopt ( s_tcp.sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof ( nodelay ) );
    struct sockaddr_in a;
    memset ( &a, 0, sizeof ( a ) );
    a.sin_family = AF_INET;
    memcpy ( &a.sin_addr, s_tcp.ip, 4 );
    a.sin_port = htons ( s_tcp.port );
    s_tcp.state = UNICARD_TCP_CONNECTING;
    if ( connect ( s_tcp.sock, (struct sockaddr *) &a, sizeof ( a ) ) == 0 ) {
        s_tcp.state = UNICARD_TCP_CONNECTED;
    } else if ( !tcp_would_block ( ) ) {
        tcp_drop ( UNICARD_TCP_FAILED );
    };
}


void unicard_tcp_close ( void ) {
    tcp_drop ( UNICARD_TCP_NONE );
}


void unicard_tcp_status ( uint8_t *out ) {
    tcp_pump ( );
    out[0] = ( s_tcp.state == UNICARD_TCP_CONNECTED ) ? 1 : 0;
    out[1] = s_tcp.state;
    out[2] = (uint8_t) ( s_tcp.rx_len & 0xff );
    out[3] = (uint8_t) ( s_tcp.rx_len >> 8 );
    memcpy ( &out[4], s_tcp.ip, 4 );
    out[8] = (uint8_t) ( s_tcp.port & 0xff );
    out[9] = (uint8_t) ( s_tcp.port >> 8 );
}


int unicard_tcp_recv ( uint8_t *dst, int max ) {
    tcp_pump ( );
    int n = max;
    if ( n > s_tcp.rx_len ) n = s_tcp.rx_len;
    if ( n <= 0 ) return 0;
    memcpy ( dst, s_tcp.rx, n );
    memmove ( s_tcp.rx, &s_tcp.rx[n], s_tcp.rx_len - n );
    s_tcp.rx_len -= n;
    return n;
}


void unicard_tcp_send ( const uint8_t *data, int len ) {
    tcp_pump ( );
    if ( s_tcp.state != UNICARD_TCP_CONNECTED ) return;
    while ( len > 0 ) {
        int n = send ( s_tcp.sock, (const char *) data, len, 0 );
        if ( n > 0 ) {
            data += n;
            len -= n;
        } else if ( n == SOCKET_ERROR && tcp_would_block ( ) ) {
            fd_set wr;
            struct timeval tv = { 0, 100000 };
            FD_ZERO ( &wr );
            FD_SET ( s_tcp.sock, &wr );
            if ( select ( (int) s_tcp.sock + 1, NULL, &wr, NULL, &tv ) <= 0 ) return;
        } else {
            tcp_drop ( UNICARD_TCP_FAILED );
            return;
        };
    };
}


void unicard_dns_request ( const char *name ) {
    struct addrinfo hints, *res = NULL;
    memset ( s_tcp.dns_ip, 0, 4 );
    if ( !name || !name[0] ) {
        s_tcp.dns_state = UNICARD_DNS_BAD_ARGS;
        return;
    };
    if ( !tcp_net_init ( ) ) {
        s_tcp.dns_state = UNICARD_DNS_UNKNOWN;
        return;
    };
    memset ( &hints, 0, sizeof ( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ( getaddrinfo ( name, NULL, &hints, &res ) || !res ) {
        s_tcp.dns_state = UNICARD_DNS_UNKNOWN;
        return;
    };
    memcpy ( s_tcp.dns_ip, &( (struct sockaddr_in *) res->ai_addr )->sin_addr, 4 );
    freeaddrinfo ( res );
    s_tcp.dns_state = UNICARD_DNS_RESOLVED;
}


void unicard_dns_result ( uint8_t *out ) {
    out[0] = s_tcp.dns_state;
    memcpy ( &out[1], s_tcp.dns_ip, 4 );
}
