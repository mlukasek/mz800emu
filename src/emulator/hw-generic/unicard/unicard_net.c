/*
 * unicard_net.c - NET A0h-A9h of the Unicard (multiplayer through a relay) on the host's sockets.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * As the Unicard mk3b firmware 0.30-0.38 (emu_NET.c; uc3 v0.37) and the MZPico NET protocol
 * (BomberNet docs/net-protocol.md):
 *  - the state the Z80 reads (room, slots, ready members, a window of NET_FRAMES frames of inputs, 4 messages)
 *    follows the relay's JSON lines; NETCREATE and NETJOIN open the connection and wait for "room" (or "error")
 *    at most 15 s, status bit 6 meanwhile; NETLEAVE closes it, as a reset of the computer;
 *  - the relay: the WebSocket (RFC 6455: masked frames, ping -> pong, close; the room in the URL
 *    /net?game=G&create=1 or &code=ABCD) to ws://api.mzpico.com/net by default, or JSON lines over TCP
 *    (BomberNet relay/relay.py, port 8766). As the card in /unicard/network.cfg: NETRELAY = name, IP or
 *    ws://name:port; NETRELAYADDR = the IP of a relay with JSON lines; NETRELAYPORT = the port of both. [UNICARD]
 *    net_relay = host[:port] of the emulator (JSON lines, default port 8766, as the MZPico fork of mz800emu)
 *    takes precedence.
 *  - Non-blocking sockets served on every NET command and status read (unicard_net_pump): the emulation never
 *    waits for the network, except the name of the relay, which the host resolves at once (the firmware waits for
 *    its DNS in the background; a name that does not resolve gives NOLINK at once, not after IN_PROGRESS).
 * The emulation is always "linked" (the card: Ethernet up and a relay set, the default one always is).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

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
#define closesocket close
#endif

#include "unicard.h"
#include "unicard_ini.h"
#include "unimgr_commands.h"
#include "unicard_net.h"

    /* NETSTATUS byte 0 */
#define NET_ST_NOLINK 0
#define NET_ST_READY 1
#define NET_ST_INROOM 2
#define NET_ST_RUNNING 3
#define NET_ST_DESYNC 4
#define NET_ST_DROPPED 5
#define NET_ST_SPECTATOR 6

#define NET_FRAMES 128          /* the window of kept frames (as the firmware) */
#define NET_SLOTS 4
#define NET_BYTES 4
#define NET_SETTINGS 16
#define NET_MSG 32
#define NET_MSGS 4              /* messages waiting for NETRECV */
#define NET_LINE 192            /* the longest JSON line, sent or received */
#define NET_WAIT_MS 15000       /* NETCREATE, NETJOIN: the relay's answer */
#define NET_TCP_PORT 8766
#define NET_WS_PORT 80
#define NET_WS_HOST "api.mzpico.com"
#define NET_HOST 64
#define NET_TXQ 8192            /* bytes the socket did not take yet */

enum { CONN_NONE, CONN_CONNECTING, CONN_HANDSHAKE, CONN_OPEN };

static struct {
    int tcp;                    /* JSON lines over TCP; else the WebSocket */
    char host[NET_HOST];
    uint16_t port;              /* 0 = the default of the transport */
} s_cfg;

static struct {
    SOCKET sock;
    int conn;
    char path[48];              /* WebSocket: /net?game=... */
    char pre[NET_LINE];         /* lines sent while connecting */
    int pre_len;
    uint8_t txq[NET_TXQ];
    int txq_len;
    char rx[NET_LINE];          /* a received line */
    int rx_len;
    int rx_skip;                /* the line was too long: dropped up to its end */
    /* WebSocket: frames from the relay (the server does not mask them) */
    int ws_hdr;                 /* 0 first byte, 1 length, 2 and 3 16-bit length, 4 data */
    int ws_fin, ws_fop, ws_mop; /* the last frame of a message, the frame's opcode, the message's opcode */
    unsigned ws_need, ws_got;
    uint8_t ws_ctl[125];        /* the data of a control frame (ping) */
    char ws_status[13];         /* the start of the answer to Upgrade */
    int ws_status_n;
    uint32_t ws_tail;           /* the last 4 bytes of the answer (the end of the headers) */
} s_link = { INVALID_SOCKET };

typedef struct {
    uint8_t from, len, data[NET_MSG];
} st_NET_MSG;

static struct {
    uint8_t state, slot, members, ready_mask, last_error;
    uint8_t slots, nbytes;
    uint8_t full_mask;          /* the slots taking part ("start"), 0 = all */
    uint8_t settings[NET_SETTINGS];
    uint8_t settings_len;
    uint16_t seed, start_frame;
    int started;
    char code[8];
    uint32_t base;              /* the oldest frame not complete yet */
    uint8_t have[NET_FRAMES];   /* the slots whose input for the frame came */
    uint8_t data[NET_FRAMES][NET_SLOTS][NET_BYTES];
    st_NET_MSG msgs[NET_MSGS];
    uint8_t msg_head, msg_tail;
    uint8_t pending_cmd;        /* NETCREATE or NETJOIN waits for the relay */
    int pending_done;
    uint8_t pending_err;
    uint8_t pending_out[4 + NET_SETTINGS];
    int pending_len;
    int64_t pending_t0;
} s_room = { .slots = NET_SLOTS, .nbytes = 1 };

static int s_test;              /* tests: no sockets */
static GByteArray *s_test_out;
static int64_t s_test_clock;


static int64_t net_ms ( void ) {
    return g_get_monotonic_time ( ) / 1000 + s_test_clock;
}


/* ---------------- configuration ---------------- */

/* [ws://]name or IP[:port][/...]: the name up to ':', '/', ';', '#' or a space (as the firmware) */
static void net_cfg_host ( const char *val ) {
    size_t n = 0;
    while ( *val && ( (unsigned char) *val <= ' ' ) ) val++;
    if ( 0 == strncmp ( val, "ws://", 5 ) ) val += 5;
    while ( ( (unsigned char) val[n] > ' ' ) && !strchr ( ":/;#", val[n] ) && ( n < sizeof ( s_cfg.host ) - 1 ) ) n++;
    if ( !n ) {
        g_strlcpy ( s_cfg.host, NET_WS_HOST, sizeof ( s_cfg.host ) );
        return;
    };
    memcpy ( s_cfg.host, val, n );
    s_cfg.host[n] = 0x00;
    if ( ':' == val[n] ) s_cfg.port = (uint16_t) atoi ( &val[n + 1] );
}


static void net_read_cfg ( void ) {
    s_cfg.tcp = 0;
    s_cfg.port = 0;
    g_strlcpy ( s_cfg.host, NET_WS_HOST, sizeof ( s_cfg.host ) );
    const char *ini = unicard_net_relay ( );
    if ( ini && ini[0] ) {
        net_cfg_host ( ini );
        s_cfg.tcp = 1;
        return;
    };
    char addr[NET_HOST] = "";
    GPtrArray *recs = unicard_config_get ( "NETWORK" );
    for ( guint i = 0; i < recs->len; i++ ) {
        const char *r = g_ptr_array_index ( recs, i );
        const char *v = r + UNICARD_INI_KEY_SIZE;
        if ( 0 == strcmp ( r, "NETRELAYADDR" ) ) {
            char ip[NET_HOST];
            struct in_addr a;
            g_strlcpy ( ip, v, sizeof ( ip ) );
            g_strstrip ( ip );
            if ( ( 1 == inet_pton ( AF_INET, ip, &a ) ) && a.s_addr ) {     /* 0.0.0.0 keeps the WebSocket */
                g_strlcpy ( addr, ip, sizeof ( addr ) );
                s_cfg.tcp = 1;
            };
        } else if ( 0 == strcmp ( r, "NETRELAYPORT" ) ) {
            s_cfg.port = (uint16_t) atoi ( v );
        } else if ( 0 == strcmp ( r, "NETRELAY" ) ) {
            net_cfg_host ( v );
        };
    };
    g_ptr_array_free ( recs, TRUE );
    if ( s_cfg.tcp ) g_strlcpy ( s_cfg.host, addr, sizeof ( s_cfg.host ) );
}


static uint16_t net_port ( void ) {
    if ( s_cfg.port ) return s_cfg.port;
    return s_cfg.tcp ? NET_TCP_PORT : NET_WS_PORT;
}


/* ---------------- JSON (flat objects only) and hex ---------------- */

static const char* json_find ( const char *s, const char *key ) {
    char pat[24];
    snprintf ( pat, sizeof ( pat ), "\"%s\"", key );
    const char *p = strstr ( s, pat );
    if ( !p ) return NULL;
    p += strlen ( pat );
    while ( ( ' ' == *p ) || ( ':' == *p ) ) p++;
    return p;
}


static long json_int ( const char *s, const char *key, long def ) {
    const char *p = json_find ( s, key );
    return p ? strtol ( p, NULL, 10 ) : def;
}


static int json_str ( const char *s, const char *key, char *out, int size ) {
    const char *p = json_find ( s, key );
    int n = 0;
    if ( p && ( '"' == *p ) ) {
        p++;
        while ( *p && ( '"' != *p ) && ( n < size - 1 ) ) out[n++] = *p++;
    };
    out[n] = 0x00;
    return n;
}


static int json_true ( const char *s, const char *key ) {
    const char *p = json_find ( s, key );
    return p && ( 0 == strncmp ( p, "true", 4 ) );
}


static int hex_digit ( char c ) {
    if ( ( c >= '0' ) && ( c <= '9' ) ) return c - '0';
    if ( ( c >= 'a' ) && ( c <= 'f' ) ) return c - 'a' + 10;
    if ( ( c >= 'A' ) && ( c <= 'F' ) ) return c - 'A' + 10;
    return 0;
}


static int hex_decode ( const char *hex, uint8_t *out, int max ) {
    int n = 0;
    while ( hex[0] && hex[1] && ( n < max ) ) {
        out[n++] = (uint8_t) ( ( hex_digit ( hex[0] ) << 4 ) | hex_digit ( hex[1] ) );
        hex += 2;
    };
    return n;
}


static void hex_encode ( const uint8_t *in, int n, char *out ) {
    static const char d[] = "0123456789abcdef";
    while ( n-- > 0 ) {
        *out++ = d[*in >> 4];
        *out++ = d[*in & 0x0f];
        in++;
    };
    *out = 0x00;
}


/* ---------------- the connection with the relay ---------------- */

static int net_init ( void ) {
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


static int net_would_block ( void ) {
#ifdef _WIN32
    int e = WSAGetLastError ( );
    return ( e == WSAEWOULDBLOCK ) || ( e == WSAEINPROGRESS );
#else
    return ( errno == EWOULDBLOCK ) || ( errno == EAGAIN ) || ( errno == EINPROGRESS );
#endif
}


static void handle_line ( const char *l );


static void ws_reset ( void ) {
    s_link.ws_hdr = 0;
    s_link.ws_fin = s_link.ws_fop = s_link.ws_mop = 0;
    s_link.ws_need = s_link.ws_got = 0;
    s_link.ws_status_n = 0;
    s_link.ws_tail = 0;
}


static void relay_close ( void ) {
    if ( s_link.sock != INVALID_SOCKET ) closesocket ( s_link.sock );
    s_link.sock = INVALID_SOCKET;
    s_link.conn = CONN_NONE;
    s_link.pre_len = 0;
    s_link.txq_len = 0;
    s_link.rx_len = 0;
    s_link.rx_skip = 0;
    ws_reset ( );
}


/* the connection is gone: a room is DROPPED, a waiting NETCREATE/NETJOIN ends with NOLINK */
static void link_lost ( void ) {
    if ( ( s_room.state >= NET_ST_INROOM ) && ( s_room.state != NET_ST_DROPPED ) ) s_room.state = NET_ST_DROPPED;
    s_room.started = 0;
    if ( s_room.pending_cmd ) {
        s_room.pending_err = UNICARD_NET_ERR_NOLINK;
        s_room.pending_done = 1;
    };
}


static void relay_drop ( void ) {
    relay_close ( );
    link_lost ( );
}


static void txq_flush ( void ) {
    while ( s_link.txq_len && ( s_link.sock != INVALID_SOCKET ) ) {
        int n = send ( s_link.sock, (const char *) s_link.txq, s_link.txq_len, 0 );
        if ( n > 0 ) {
            memmove ( s_link.txq, &s_link.txq[n], (size_t) ( s_link.txq_len - n ) );
            s_link.txq_len -= n;
        } else {
            if ( !net_would_block ( ) ) relay_drop ( );
            return;
        };
    };
}


/* bytes to the relay: the tests collect them, else through the queue into the socket */
static int raw_send ( const uint8_t *data, int len ) {
    if ( s_test ) {
        if ( !s_test_out ) s_test_out = g_byte_array_new ( );
        g_byte_array_append ( s_test_out, data, (guint) len );
        return 1;
    };
    if ( s_link.sock == INVALID_SOCKET ) return 0;
    if ( s_link.txq_len + len > NET_TXQ ) return 0;
    memcpy ( &s_link.txq[s_link.txq_len], data, (size_t) len );
    s_link.txq_len += len;
    txq_flush ( );
    return 1;
}


/* WebSocket: one masked frame (FIN) */
static int ws_send_frame ( uint8_t op, const uint8_t *data, int len ) {
    uint8_t buf[NET_LINE + 8];
    int h = 0;
    if ( len > NET_LINE ) return 0;
    uint32_t m = g_random_int ( );
    buf[h++] = (uint8_t) ( 0x80 | op );
    if ( len < 126 ) {
        buf[h++] = (uint8_t) ( 0x80 | len );
    } else {
        buf[h++] = 0x80 | 126;
        buf[h++] = (uint8_t) ( len >> 8 );
        buf[h++] = (uint8_t) len;
    };
    for ( int i = 0; i < 4; i++ ) buf[h++] = (uint8_t) ( m >> ( 8 * i ) );
    for ( int i = 0; i < len; i++ ) buf[h + i] = data[i] ^ buf[h - 4 + ( i & 3 )];
    return raw_send ( buf, h + len );
}


/* WebSocket: the lines sent while the connection was being opened, each as a text frame */
static void ws_flush_pre ( void ) {
    int s = 0;
    for ( int i = 0; i < s_link.pre_len; i++ ) {
        if ( '\n' == s_link.pre[i] ) {
            ws_send_frame ( 1, (const uint8_t *) &s_link.pre[s], i - s );
            s = i + 1;
        };
    };
    s_link.pre_len = 0;
}


/* WebSocket: the next received byte (the answer to Upgrade, then frames); 0 = close the connection */
static int ws_byte ( uint8_t b ) {
    if ( CONN_HANDSHAKE == s_link.conn ) {
        if ( s_link.ws_status_n < (int) sizeof ( s_link.ws_status ) - 1 ) s_link.ws_status[s_link.ws_status_n++] = (char) b;
        s_link.ws_tail = ( s_link.ws_tail << 8 ) | b;
        if ( 0x0d0a0d0a != s_link.ws_tail ) return 1;      /* the headers of the answer do not matter */
        s_link.ws_status[s_link.ws_status_n] = 0x00;
        if ( 0 != strncmp ( s_link.ws_status, "HTTP/1.1 101", 12 ) ) return 0;     /* refused */
        s_link.conn = CONN_OPEN;
        s_link.ws_hdr = 0;
        ws_flush_pre ( );
        return 1;
    };
    if ( CONN_OPEN != s_link.conn ) return 1;
    switch ( s_link.ws_hdr ) {
        case 0:
            s_link.ws_fin = ( b & 0x80 ) ? 1 : 0;
            s_link.ws_fop = b & 0x0f;
            if ( ( 1 == s_link.ws_fop ) || ( 2 == s_link.ws_fop ) ) {      /* a new message */
                s_link.ws_mop = s_link.ws_fop;
                s_link.rx_len = 0;
                s_link.rx_skip = 0;
            };
            s_link.ws_hdr = 1;
            return 1;
        case 1:
            if ( b & 0x80 ) return 0;                       /* a masked frame from the server */
            b &= 0x7f;
            s_link.ws_got = 0;
            if ( 126 == b ) {
                s_link.ws_hdr = 2;
                return 1;
            };
            if ( 127 == b ) return 0;                       /* a 64-bit length: the relay sends short messages */
            s_link.ws_need = b;
            s_link.ws_hdr = 4;
            break;
        case 2:
            s_link.ws_need = (unsigned) b << 8;
            s_link.ws_hdr = 3;
            return 1;
        case 3:
            s_link.ws_need |= b;
            s_link.ws_hdr = 4;
            break;
        default:
            if ( s_link.ws_fop >= 8 ) {
                if ( s_link.ws_got < sizeof ( s_link.ws_ctl ) ) s_link.ws_ctl[s_link.ws_got] = b;
            } else if ( ( 1 == s_link.ws_mop ) && !s_link.rx_skip ) {
                if ( s_link.rx_len < NET_LINE - 1 ) s_link.rx[s_link.rx_len++] = (char) b;
                else s_link.rx_skip = 1;
            };
            s_link.ws_got++;
            break;
    };
    if ( s_link.ws_got < s_link.ws_need ) return 1;
    /* the end of the frame */
    s_link.ws_hdr = 0;
    unsigned n = ( s_link.ws_got < sizeof ( s_link.ws_ctl ) ) ? s_link.ws_got : sizeof ( s_link.ws_ctl );
    if ( 9 == s_link.ws_fop ) {                             /* ping -> pong with the same data */
        ws_send_frame ( 10, s_link.ws_ctl, (int) n );
    } else if ( 8 == s_link.ws_fop ) {                      /* close: answer and close */
        ws_send_frame ( 8, s_link.ws_ctl, ( s_link.ws_got >= 2 ) ? 2 : 0 );
        return 0;
    } else if ( ( s_link.ws_fop <= 2 ) && s_link.ws_fin ) { /* a whole message */
        if ( ( 1 == s_link.ws_mop ) && !s_link.rx_skip && s_link.rx_len ) {
            s_link.rx[s_link.rx_len] = 0x00;
            handle_line ( s_link.rx );
        };
        s_link.rx_len = 0;
        s_link.rx_skip = 0;
    };
    return 1;
}


/* a received byte; 0 = close the connection */
static int rx_byte ( uint8_t b ) {
    if ( !s_cfg.tcp ) return ws_byte ( b );
    if ( '\n' == b ) {
        if ( !s_link.rx_skip && s_link.rx_len ) {
            s_link.rx[s_link.rx_len] = 0x00;
            handle_line ( s_link.rx );
        };
        s_link.rx_len = 0;
        s_link.rx_skip = 0;
    } else if ( ( '\r' != b ) && !s_link.rx_skip ) {
        if ( s_link.rx_len < NET_LINE - 1 ) s_link.rx[s_link.rx_len++] = (char) b;
        else s_link.rx_skip = 1;
    };
    return 1;
}


static void rx_bytes ( const uint8_t *data, int len ) {
    for ( int i = 0; ( i < len ) && ( CONN_NONE != s_link.conn ); i++ ) {
        if ( !rx_byte ( data[i] ) ) {
            relay_drop ( );
            return;
        };
    };
}


/* the TCP connection is open: the WebSocket asks for Upgrade (Sec-WebSocket-Accept not checked, as the firmware) */
static void relay_connected ( void ) {
    if ( s_cfg.tcp ) {
        s_link.conn = CONN_OPEN;
        if ( s_link.pre_len && raw_send ( (const uint8_t *) s_link.pre, s_link.pre_len ) ) s_link.pre_len = 0;
        return;
    };
    uint8_t raw[16];
    for ( int i = 0; i < 16; i++ ) raw[i] = (uint8_t) g_random_int ( );
    gchar *key = g_base64_encode ( raw, sizeof ( raw ) );
    uint16_t port = net_port ( );
    gchar *host = ( NET_WS_PORT == port ) ? g_strdup ( s_cfg.host ) : g_strdup_printf ( "%s:%u", s_cfg.host, port );
    gchar *req = g_strdup_printf ( "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: Unicard (mz800emu)\r\n\r\n",
                                   s_link.path, host, key );
    s_link.conn = CONN_HANDSHAKE;
    ws_reset ( );
    if ( !raw_send ( (const uint8_t *) req, (int) strlen ( req ) ) ) relay_drop ( );
    g_free ( req );
    g_free ( host );
    g_free ( key );
}


/* opens the connection; path = the WebSocket URL path (the room) */
static int relay_open ( const char *path ) {
    relay_close ( );
    net_read_cfg ( );
    g_strlcpy ( s_link.path, path, sizeof ( s_link.path ) );
    s_link.conn = CONN_CONNECTING;
    if ( s_test ) {
        relay_connected ( );
        return CONN_NONE != s_link.conn;
    };
    if ( !net_init ( ) ) {
        s_link.conn = CONN_NONE;
        return 0;
    };
    struct addrinfo hints, *res = NULL;
    char port[8];
    memset ( &hints, 0, sizeof ( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf ( port, sizeof ( port ), "%u", net_port ( ) );
    if ( getaddrinfo ( s_cfg.host, port, &hints, &res ) || !res ) {
        fprintf ( stderr, "WARNING: Unicard NET: the relay %s not found\n", s_cfg.host );
        s_link.conn = CONN_NONE;
        return 0;
    };
    SOCKET s = socket ( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( s == INVALID_SOCKET ) {
        freeaddrinfo ( res );
        s_link.conn = CONN_NONE;
        return 0;
    };
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket ( s, FIONBIO, &nb );
#else
    fcntl ( s, F_SETFL, fcntl ( s, F_GETFL, 0 ) | O_NONBLOCK );
#endif
    int one = 1;
    setsockopt ( s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof ( one ) );   /* small lines every frame */
    int r = connect ( s, res->ai_addr, (int) res->ai_addrlen );
    freeaddrinfo ( res );
    s_link.sock = s;
    if ( 0 == r ) {
        relay_connected ( );
        return CONN_NONE != s_link.conn;
    };
    if ( !net_would_block ( ) ) {
        relay_close ( );
        return 0;
    };
    return 1;
}


/* one JSON line; while connecting it waits in pre */
static int relay_send ( const char *line ) {
    int len = (int) strlen ( line );
    if ( ( CONN_NONE != s_link.conn ) && ( CONN_OPEN != s_link.conn ) ) {
        if ( s_link.pre_len + len + 1 > NET_LINE ) return 0;
        memcpy ( &s_link.pre[s_link.pre_len], line, (size_t) len );
        s_link.pre[s_link.pre_len + len] = '\n';
        s_link.pre_len += len + 1;
        return 1;
    };
    if ( ( CONN_OPEN != s_link.conn ) || ( len >= NET_LINE ) ) return 0;
    if ( !s_cfg.tcp ) return ws_send_frame ( 1, (const uint8_t *) line, len );
    char buf[NET_LINE + 1];
    memcpy ( buf, line, (size_t) len );
    buf[len] = '\n';
    return raw_send ( (const uint8_t *) buf, len + 1 );
}


/* NETLEAVE: the last line, the WebSocket's close frame, then close */
static void relay_leave ( void ) {
    relay_send ( "{\"op\":\"leave\"}" );
    if ( !s_cfg.tcp && ( CONN_OPEN == s_link.conn ) ) {
        static const uint8_t normal[2] = { 0x03, 0xe8 };    /* 1000: a normal closure */
        ws_send_frame ( 8, normal, 2 );
    };
    txq_flush ( );
    relay_close ( );
}


void unicard_net_pump ( void ) {
    if ( !s_test && ( s_link.sock != INVALID_SOCKET ) ) {
        if ( CONN_CONNECTING == s_link.conn ) {
            fd_set wr, ex;
            struct timeval tv = { 0, 0 };
            FD_ZERO ( &wr );
            FD_ZERO ( &ex );
            FD_SET ( s_link.sock, &wr );
            FD_SET ( s_link.sock, &ex );
            if ( select ( (int) s_link.sock + 1, NULL, &wr, &ex, &tv ) > 0 ) {
                int err = 0;
                socklen_t len = sizeof ( err );
                if ( FD_ISSET ( s_link.sock, &ex )
                     || getsockopt ( s_link.sock, SOL_SOCKET, SO_ERROR, (char *) &err, &len ) || err ) {
                    relay_drop ( );
                } else {
                    relay_connected ( );
                };
            };
        };
        if ( ( s_link.sock != INVALID_SOCKET ) && ( s_link.conn >= CONN_HANDSHAKE ) ) {
            uint8_t buf[2048];
            txq_flush ( );
            for ( int k = 0; ( k < 64 ) && ( s_link.sock != INVALID_SOCKET ); k++ ) {
                int n = recv ( s_link.sock, (char *) buf, sizeof ( buf ), 0 );
                if ( n > 0 ) {
                    rx_bytes ( buf, n );
                } else {
                    if ( ( 0 == n ) || !net_would_block ( ) ) relay_drop ( );   /* the relay closed it, or an error */
                    break;
                };
            };
        };
    };
    if ( s_room.pending_cmd && !s_room.pending_done && ( ( net_ms ( ) - s_room.pending_t0 ) > NET_WAIT_MS ) ) {
        relay_drop ( );
    };
}


/* ---------------- the state ---------------- */

static void frames_clear ( void ) {
    s_room.base = 0;
    memset ( s_room.have, 0, sizeof ( s_room.have ) );
    memset ( s_room.data, 0, sizeof ( s_room.data ) );
}


static void store_input ( uint32_t frame, int slot, const uint8_t *data ) {
    if ( ( slot < 0 ) || ( slot >= NET_SLOTS ) ) return;
    if ( ( frame < s_room.base ) || ( frame >= s_room.base + NET_FRAMES ) ) return;
    uint32_t i = frame % NET_FRAMES;
    memcpy ( s_room.data[i][slot], data, NET_BYTES );
    s_room.have[i] |= (uint8_t) ( 1 << slot );
}


static void handle_line ( const char *l ) {
    char op[16];
    json_str ( l, "op", op, sizeof ( op ) );

    if ( 0 == strcmp ( op, "room" ) ) {
        char hex[2 * NET_SETTINGS + 1];
        long slot = json_int ( l, "slot", -1 );
        json_str ( l, "code", s_room.code, sizeof ( s_room.code ) );
        s_room.slots = (uint8_t) json_int ( l, "slots", s_room.slots );
        s_room.nbytes = (uint8_t) json_int ( l, "bytes", s_room.nbytes );
        if ( ( s_room.slots < 1 ) || ( s_room.slots > NET_SLOTS ) ) s_room.slots = NET_SLOTS;
        if ( ( s_room.nbytes < 1 ) || ( s_room.nbytes > NET_BYTES ) ) s_room.nbytes = 1;
        if ( json_str ( l, "settings", hex, sizeof ( hex ) ) ) {
            s_room.settings_len = (uint8_t) hex_decode ( hex, s_room.settings, NET_SETTINGS );
        };
        if ( json_true ( l, "spectator" ) || ( slot < 0 ) ) {
            s_room.slot = 0xff;
            s_room.state = NET_ST_SPECTATOR;
        } else {
            s_room.slot = (uint8_t) slot;
            s_room.state = NET_ST_INROOM;
        };
        s_room.members = 1;
        s_room.ready_mask = 0;
        s_room.started = 0;
        s_room.full_mask = 0;
        frames_clear ( );
        if ( cmdNETCREATE == s_room.pending_cmd ) {
            memcpy ( s_room.pending_out, s_room.code, 4 );
            s_room.pending_out[4] = 0x0d;
            s_room.pending_out[5] = s_room.slot;
            s_room.pending_len = 6;
            s_room.pending_done = 1;
        } else if ( cmdNETJOIN == s_room.pending_cmd ) {
            s_room.pending_out[0] = s_room.slot;
            s_room.pending_out[1] = s_room.slots;
            s_room.pending_out[2] = s_room.nbytes;
            s_room.pending_out[3] = s_room.settings_len;
            memcpy ( &s_room.pending_out[4], s_room.settings, NET_SETTINGS );
            s_room.pending_len = 4 + NET_SETTINGS;
            s_room.pending_done = 1;
        };
    } else if ( 0 == strcmp ( op, "members" ) ) {
        s_room.members = (uint8_t) json_int ( l, "count", s_room.members );
        s_room.ready_mask = (uint8_t) json_int ( l, "ready", s_room.ready_mask );
    } else if ( 0 == strcmp ( op, "start" ) ) {
        s_room.seed = (uint16_t) json_int ( l, "seed", 0 );
        s_room.start_frame = (uint16_t) json_int ( l, "frame", 0 );
        s_room.started = 1;
        s_room.full_mask = (uint8_t) json_int ( l, "mask", ( 1 << s_room.slots ) - 1 );
        if ( NET_ST_SPECTATOR != s_room.state ) s_room.state = NET_ST_RUNNING;
        frames_clear ( );
        s_room.base = s_room.start_frame;
    } else if ( 0 == strcmp ( op, "input" ) ) {
        char hex[2 * NET_BYTES + 1];
        uint8_t data[NET_BYTES] = { 0 };
        json_str ( l, "data", hex, sizeof ( hex ) );
        hex_decode ( hex, data, NET_BYTES );
        store_input ( (uint32_t) json_int ( l, "frame", 0 ), (int) json_int ( l, "slot", -1 ), data );
    } else if ( 0 == strcmp ( op, "desync" ) ) {
        s_room.state = NET_ST_DESYNC;
    } else if ( 0 == strcmp ( op, "dropped" ) ) {
        s_room.state = NET_ST_DROPPED;
        s_room.started = 0;
    } else if ( 0 == strcmp ( op, "msg" ) ) {
        if ( (uint8_t) ( s_room.msg_head - s_room.msg_tail ) < NET_MSGS ) {
            char hex[2 * NET_MSG + 1];
            st_NET_MSG *m = &s_room.msgs[s_room.msg_head % NET_MSGS];
            json_str ( l, "data", hex, sizeof ( hex ) );
            m->from = (uint8_t) json_int ( l, "from", 0xff );
            memset ( m->data, 0, NET_MSG );
            m->len = (uint8_t) hex_decode ( hex, m->data, NET_MSG );
            s_room.msg_head++;
        };
    } else if ( 0 == strcmp ( op, "error" ) ) {
        s_room.last_error = (uint8_t) json_int ( l, "code", UNICARD_NET_ERR_PARAM );
        if ( s_room.pending_cmd ) {
            s_room.pending_err = s_room.last_error;
            s_room.pending_done = 1;
        };
    };
}


/* the newest frame whose inputs of all playing slots (and of every frame before) came; FFFFh = none */
static uint32_t avail_frame ( void ) {
    uint8_t full = s_room.full_mask ? s_room.full_mask : (uint8_t) ( ( 1 << s_room.slots ) - 1 );
    while ( ( s_room.base < 0xffff ) && ( ( s_room.have[s_room.base % NET_FRAMES] & full ) == full ) ) {
        s_room.have[s_room.base % NET_FRAMES] = 0;        /* free for the frame entering the window */
        s_room.base++;
    };
    return s_room.base ? s_room.base - 1 : 0xffff;
}


/* ---------------- the commands ---------------- */

int unicard_net_is_cmd ( uint8_t cmd ) {
    return ( cmd >= cmdNETSTATUS ) && ( cmd <= cmdNETRECV );
}


const char* unicard_net_param_format ( uint8_t cmd ) {
    switch ( cmd ) {
        case cmdNETCREATE: return "BBBBBBBBBBBBBBBBBBBBBBB";   /* game, build, slots, bytes, len, 16 B settings */
        case cmdNETJOIN: return "BBBBS";                       /* game, build, the room's code */
        case cmdNETREADY: return "B";
        case cmdNETSEND: return "BBBBBB";                       /* frame, 4 B input */
        case cmdNETPOLL: return "BB";
        case cmdNETHASH: return "BBBB";
        case cmdNETMSG: return "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";  /* to, len, 32 B */
        default: return "";
    };
}


static int start_pending ( uint8_t cmd, const char *line, const char *path ) {
    if ( !relay_open ( path ) ) {
        s_room.last_error = UNICARD_NET_ERR_NOLINK;
        return UNICARD_NET_ERR_NOLINK;
    };
    relay_send ( line );
    s_room.pending_cmd = cmd;
    s_room.pending_done = 0;
    s_room.pending_err = 0;
    s_room.pending_len = 0;
    s_room.pending_t0 = net_ms ( );
    return -1;
}


int unicard_net_exec ( uint8_t cmd, const uint8_t *p, uint8_t *out, int *out_len ) {
    char line[NET_LINE];
    *out_len = 0;
    unicard_net_pump ( );

    switch ( cmd ) {
        case cmdNETSTATUS:
        {
            uint32_t av = ( ( NET_ST_RUNNING == s_room.state ) || ( NET_ST_SPECTATOR == s_room.state ) ) ? avail_frame ( ) : 0xffff;
            out[0] = ( NET_ST_NOLINK == s_room.state ) ? NET_ST_READY : s_room.state;
            out[1] = s_room.slot;
            out[2] = s_room.members;
            out[3] = s_room.ready_mask;
            out[4] = 0; /* rtt/10 ms: not measured (as the firmware) */
            out[5] = ( 0xffff == av ) ? 0 : (uint8_t) ( ( av + 1 > s_room.start_frame ) ? ( av + 1 - s_room.start_frame ) : 0 );
            out[6] = (uint8_t) ( s_room.msg_head - s_room.msg_tail );
            out[7] = s_room.last_error;
            *out_len = 8;
            return 0;
        }

        case cmdNETCREATE:
        {
            char hex[2 * NET_SETTINGS + 1], path[48];
            int len = ( p[6] > NET_SETTINGS ) ? NET_SETTINGS : p[6];
            unsigned game = p[0] | ( p[1] << 8 );
            if ( ( p[4] < 1 ) || ( p[4] > NET_SLOTS ) || ( p[5] < 1 ) || ( p[5] > NET_BYTES ) ) return UNICARD_NET_ERR_PARAM;
            hex_encode ( &p[7], len, hex );
            snprintf ( line, sizeof ( line ), "{\"op\":\"create\",\"game\":%u,\"build\":%u,\"slots\":%u,\"bytes\":%u,\"settings\":\"%s\"}",
                       game, (unsigned) ( p[2] | ( p[3] << 8 ) ), (unsigned) p[4], (unsigned) p[5], hex );
            snprintf ( path, sizeof ( path ), "/net?game=%u&create=1", game );
            return start_pending ( cmd, line, path );
        }

        case cmdNETJOIN:
        {
            char code[8], path[48];
            const char *s = (const char *) &p[4];
            unsigned game = p[0] | ( p[1] << 8 );
            int i;
            for ( i = 0; s[i] && ( i < (int) sizeof ( code ) - 1 ); i++ ) {     /* letters and digits: no broken JSON */
                if ( !g_ascii_isalnum ( s[i] ) ) return UNICARD_NET_ERR_PARAM;
                code[i] = s[i];
            };
            code[i] = 0x00;
            snprintf ( line, sizeof ( line ), "{\"op\":\"join\",\"game\":%u,\"build\":%u,\"code\":\"%s\"}",
                       game, (unsigned) ( p[2] | ( p[3] << 8 ) ), code );
            snprintf ( path, sizeof ( path ), "/net?game=%u&code=%s", game, code );
            return start_pending ( cmd, line, path );
        }

        case cmdNETLEAVE:
            if ( s_room.state < NET_ST_INROOM ) return UNICARD_NET_ERR_NOROOM;
            relay_leave ( );
            s_room.state = NET_ST_READY;
            s_room.started = 0;
            s_room.members = 0;
            s_room.ready_mask = 0;
            s_room.full_mask = 0;
            return 0;

        case cmdNETREADY:
            if ( ( NET_ST_INROOM != s_room.state ) && ( NET_ST_RUNNING != s_room.state ) ) return UNICARD_NET_ERR_NOROOM;
            snprintf ( line, sizeof ( line ), "{\"op\":\"ready\",\"ready\":%u}", p[0] ? 1u : 0u );
            relay_send ( line );
            if ( s_room.started ) {
                out[0] = (uint8_t) s_room.seed;
                out[1] = (uint8_t) ( s_room.seed >> 8 );
                out[2] = (uint8_t) s_room.start_frame;
                out[3] = (uint8_t) ( s_room.start_frame >> 8 );
            } else {
                memset ( out, 0xff, 4 );
            };
            *out_len = 4;
            return 0;

        case cmdNETSEND:
        {
            char hex[2 * NET_BYTES + 1];
            uint8_t data[NET_BYTES] = { 0 };
            uint32_t frame = p[0] | ( p[1] << 8 );
            if ( NET_ST_RUNNING != s_room.state ) return UNICARD_NET_ERR_NOROOM;
            memcpy ( data, &p[2], s_room.nbytes );
            hex_encode ( data, s_room.nbytes, hex );
            store_input ( frame, s_room.slot, data );
            snprintf ( line, sizeof ( line ), "{\"op\":\"input\",\"frame\":%u,\"data\":\"%s\"}", (unsigned) frame, hex );
            return relay_send ( line ) ? 0 : UNICARD_NET_ERR_FULL;
        }

        case cmdNETPOLL:
        {
            uint32_t frame = p[0] | ( p[1] << 8 );
            if ( ( NET_ST_RUNNING != s_room.state ) && ( NET_ST_SPECTATOR != s_room.state ) ) return UNICARD_NET_ERR_NOROOM;
            uint32_t av = avail_frame ( );
            out[0] = (uint8_t) av;
            out[1] = (uint8_t) ( av >> 8 );
            memset ( &out[2], 0, NET_SLOTS * NET_BYTES );
            if ( ( 0xffff != av ) && ( frame <= av ) && ( frame + NET_FRAMES > s_room.base ) ) {
                for ( int s = 0; s < s_room.slots; s++ ) {
                    memcpy ( &out[2 + s * s_room.nbytes], s_room.data[frame % NET_FRAMES][s], s_room.nbytes );
                };
            };
            *out_len = 2 + s_room.slots * s_room.nbytes;
            return 0;
        }

        case cmdNETHASH:
            if ( NET_ST_RUNNING != s_room.state ) return UNICARD_NET_ERR_NOROOM;
            snprintf ( line, sizeof ( line ), "{\"op\":\"hash\",\"frame\":%u,\"hash\":%u}",
                       (unsigned) ( p[0] | ( p[1] << 8 ) ), (unsigned) ( p[2] | ( p[3] << 8 ) ) );
            relay_send ( line );
            return 0;

        case cmdNETMSG:
        {
            char hex[2 * NET_MSG + 1];
            int len = ( p[1] > NET_MSG ) ? NET_MSG : p[1];
            if ( s_room.state < NET_ST_INROOM ) return UNICARD_NET_ERR_NOROOM;
            hex_encode ( &p[2], len, hex );
            snprintf ( line, sizeof ( line ), "{\"op\":\"msg\",\"to\":%d,\"data\":\"%s\"}",
                       ( 0xff == p[0] ) ? -1 : (int) p[0], hex );
            relay_send ( line );
            return 0;
        }

        case cmdNETRECV:
            memset ( out, 0, 2 + NET_MSG );
            if ( s_room.msg_head != s_room.msg_tail ) {
                st_NET_MSG *m = &s_room.msgs[s_room.msg_tail % NET_MSGS];
                out[0] = m->from;
                out[1] = m->len;
                memcpy ( &out[2], m->data, NET_MSG );
                s_room.msg_tail++;
            } else {
                out[0] = 0xff;
            };
            *out_len = 2 + NET_MSG;
            return 0;
    };
    return UNICARD_NET_ERR_PARAM;
}


int unicard_net_async_poll ( uint8_t *out, int *out_len ) {
    *out_len = 0;
    unicard_net_pump ( );
    if ( !s_room.pending_cmd ) return 1;
    if ( !s_room.pending_done ) return 0;
    s_room.pending_cmd = 0;
    if ( s_room.pending_err ) return s_room.pending_err;
    memcpy ( out, s_room.pending_out, (size_t) s_room.pending_len );
    *out_len = s_room.pending_len;
    return 1;
}


void unicard_net_cancel ( void ) {
    s_room.pending_cmd = 0;
    s_room.pending_done = 0;
}


void unicard_net_reset ( void ) {
    relay_close ( );
    memset ( &s_room, 0, sizeof ( s_room ) );
    s_room.slots = NET_SLOTS;
    s_room.nbytes = 1;
}


/* ---------------- tests ---------------- */

void unicard_net_test_mode ( int on ) {
    unicard_net_reset ( );
    s_test = on;
    s_test_clock = 0;
    if ( s_test_out ) g_byte_array_set_size ( s_test_out, 0 );
}


void unicard_net_test_recv ( const uint8_t *data, int len ) {
    rx_bytes ( data, len );
}


int unicard_net_test_sent ( uint8_t *buf, int max ) {
    if ( !s_test_out ) return 0;
    int n = ( (int) s_test_out->len < max ) ? (int) s_test_out->len : max;
    memcpy ( buf, s_test_out->data, (size_t) n );
    g_byte_array_remove_range ( s_test_out, 0, (guint) n );
    return n;
}


void unicard_net_test_advance ( int ms ) {
    s_test_clock += ms;
}
