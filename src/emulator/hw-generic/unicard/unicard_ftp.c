/*
 * unicard_ftp.c - the volume ftp: of the Unicard against real FTP servers.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * As the FTP client of the mk3b firmware 0.38 (emu_FTPC.c, UC-UniMgr docs/api.md 4.10): the servers are the sections
 * of /unicard/ftp.cfg on the emulated SD card (HOST, PORT, USER, PASS; ; and # comments), paths "ftp:/server/path".
 * Every request logs in, does one thing and quits: LIST (the Unix and the DOS form, as FILELIST text), RETR
 * (checked against SIZE), STOR, MKD, DELE (a directory: RMD), RNFR + RNTO; passive mode (EPSV, else PASV to the
 * address of the control connection), TYPE I; 10 s without an answer = TIMEOUT.
 * A request runs in a thread of its own with blocking sockets: the emulation never waits for the network, it takes
 * the result at the card's status reads (unimgr.c). The folder of MZ800EMU_UNICARD_FTP (unicard.c) takes the place
 * of the servers in the tests of the manager.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
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
#include <glib.h>
#include <glib/gstdio.h>

#include "unicard.h"
#include "unimgr_commands.h"
#include "unicard_ftp.h"

#define FTP_TIMEOUT_MS  10000
#define FTP_NAME        17          /* a server's name (a section), with its 0 */
#define FTP_PATH        256

#define OP_LIST         1
#define OP_GET          2
#define OP_PUT          3
#define OP_CMD          4

typedef struct {
    char host[64];
    int port;
    char user[32];
    char pass[32];
} st_FTPSRV;

typedef struct {
    int op, what;
    st_FTPSRV srv;
    char path[FTP_PATH], path2[FTP_PATH];
    char *local;                    /* the host's file: RETR into it, STOR from it (g_free) */
    char *list;                     /* the listing (FILELIST text) */
    int list_len, list_full;
    gint done, result, cancel;      /* atomic */
    volatile guint64 moved, total;
    GThread *thread;
    /* the control connection */
    SOCKET cs;
    char rbuf[2048];
    int rlen;
    char line[512];
} st_FTPREQ;

static st_FTPREQ *s_req;            /* the request (running or finished) */
static st_FTPREQ *s_zombie;         /* a cancelled request whose thread still runs */

#define LIST_MAX 8192


/* ---------------- ftp.cfg ---------------- */

static char* trim ( char *s ) {
    while ( *s && ( (unsigned char) *s <= ' ' ) ) s++;
    char *e = s + strlen ( s );
    while ( ( e > s ) && ( (unsigned char) e[-1] <= ' ' ) ) *--e = 0x00;
    return s;
}


/* Calls fn for each section (key NULL) and each KEY=value of ftp.cfg. */
static void cfg_scan ( void ( *fn ) ( const char *sec, const char *key, const char *val, void *arg ), void *arg ) {
    char *host = unicard_jail_resolve ( "/unicard/ftp.cfg" );
    gchar *text = NULL;
    if ( !host || !g_file_get_contents ( host, &text, NULL, NULL ) ) {
        g_free ( host );
        return;
    };
    g_free ( host );
    char sec[FTP_NAME] = "";
    gchar **lines = g_strsplit_set ( text, "\r\n", -1 );
    for ( int i = 0; lines[i]; i++ ) {
        char *s = trim ( lines[i] );
        if ( !*s || ( *s == ';' ) || ( *s == '#' ) ) continue;
        if ( *s == '[' ) {
            char *e = strchr ( s, ']' );
            sec[0] = 0x00;
            if ( !e ) continue;
            *e = 0x00;
            s = trim ( s + 1 );
            if ( !*s || ( strlen ( s ) >= FTP_NAME ) || strchr ( s, '/' ) ) continue;
            strcpy ( sec, s );
            fn ( sec, NULL, NULL, arg );
            continue;
        };
        char *v = strchr ( s, '=' );
        if ( !sec[0] || !v ) continue;
        *v++ = 0x00;
        fn ( sec, trim ( s ), trim ( v ), arg );
    };
    g_strfreev ( lines );
    g_free ( text );
}


typedef struct {
    const char *want;
    st_FTPSRV *srv;
    int found;
} st_FIND;


static void find_fn ( const char *sec, const char *key, const char *val, void *arg ) {
    st_FIND *f = arg;
    if ( g_ascii_strcasecmp ( sec, f->want ) ) return;
    if ( !key ) {
        f->found = 1;
        return;
    };
    if ( !g_ascii_strcasecmp ( key, "HOST" ) ) g_strlcpy ( f->srv->host, val, sizeof ( f->srv->host ) );
    else if ( !g_ascii_strcasecmp ( key, "PORT" ) ) f->srv->port = atoi ( val );
    else if ( !g_ascii_strcasecmp ( key, "USER" ) ) g_strlcpy ( f->srv->user, val, sizeof ( f->srv->user ) );
    else if ( !g_ascii_strcasecmp ( key, "PASS" ) ) g_strlcpy ( f->srv->pass, val, sizeof ( f->srv->pass ) );
}


static int cfg_find ( const char *name, st_FTPSRV *srv ) {
    memset ( srv, 0, sizeof ( *srv ) );
    strcpy ( srv->user, "anonymous" );
    srv->port = 21;
    st_FIND f = { name, srv, 0 };
    cfg_scan ( find_fn, &f );
    if ( srv->port <= 0 ) srv->port = 21;
    return f.found && srv->host[0];
}


typedef struct {
    char *out;
    size_t max, len;
    int full, count;
} st_SERVERS;


static void servers_fn ( const char *sec, const char *key, const char *val, void *arg ) {
    st_SERVERS *s = arg;
    (void) val;
    if ( key ) return;
    s->count++;
    if ( !s->out ) return;
    size_t n = strlen ( sec );
    if ( s->len + n + 4 > s->max ) {
        s->full = 1;
        return;
    };
    memcpy ( &s->out[s->len], sec, n );
    s->len += n;
    memcpy ( &s->out[s->len], "/\r0\r", 4 );
    s->len += 4;
}


int unicard_ftpc_has_server ( void ) {
    st_SERVERS s = { NULL, 0, 0, 0, 0 };
    cfg_scan ( servers_fn, &s );
    return s.count > 0;
}


int unicard_ftpc_servers ( char *out, size_t max ) {
    st_SERVERS s = { out, max, 0, 0, 0 };
    cfg_scan ( servers_fn, &s );
    return s.full ? -1 : (int) s.len;
}


/* "ftp:/name/dir/file": the server's name and the path on it ("/" its root). 0 = no server in the path */
static int split ( const char *p, char *name, char *out ) {
    size_t n = 0;
    p += 4;
    while ( *p == '/' ) p++;
    while ( *p && ( *p != '/' ) ) {
        if ( n >= FTP_NAME - 1 ) return 0;
        name[n++] = *p++;
    };
    name[n] = 0x00;
    if ( !n ) return 0;
    while ( ( *p == '/' ) && ( p[1] == '/' ) ) p++;
    if ( !*p || !p[1] ) {
        strcpy ( out, "/" );
        return 1;
    };
    size_t len = strlen ( p );
    while ( ( len > 1 ) && ( p[len - 1] == '/' ) ) len--;
    if ( len >= FTP_PATH ) return 0;
    memcpy ( out, p, len );
    out[len] = 0x00;
    return 1;
}


/* ---------------- the listing ---------------- */

static void list_add ( st_FTPREQ *r, const char *name, size_t len, int dir, guint64 size ) {
    char num[24];
    if ( !len || r->list_full ) return;
    int nl = snprintf ( num, sizeof ( num ), "%" G_GUINT64_FORMAT, size );
    if ( (size_t) r->list_len + len + 1 + 1 + (size_t) nl + 1 > LIST_MAX ) {
        r->list_full = 1;
        return;
    };
    memcpy ( &r->list[r->list_len], name, len );
    r->list_len += (int) len;
    if ( dir ) r->list[r->list_len++] = '/';
    r->list[r->list_len++] = 0x0d;
    memcpy ( &r->list[r->list_len], num, (size_t) nl );
    r->list_len += nl;
    r->list[r->list_len++] = 0x0d;
}


static int is_month ( const char *t, size_t n ) {
    static const char m[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if ( n != 3 ) return 0;
    for ( int i = 0; i < 36; i += 3 ) if ( !g_ascii_strncasecmp ( t, &m[i], 3 ) ) return 1;
    return 0;
}


/* One line of LIST: Unix "ls -l" (the name after month, day and time or year) or DOS. As the firmware. */
static void list_line ( st_FTPREQ *r, char *l, size_t n ) {
    char *t[12];
    size_t tl[12];
    int k = 0;
    char *p = l, *end = l + n, *name;
    size_t len;
    while ( ( k < 12 ) && ( p < end ) ) {
        while ( ( p < end ) && ( *p == ' ' ) ) p++;
        if ( p >= end ) break;
        t[k] = p;
        while ( ( p < end ) && ( *p != ' ' ) ) p++;
        tl[k] = (size_t) ( p - t[k] );
        k++;
    };
    if ( k < 4 ) return;
    if ( isdigit ( (unsigned char) t[0][0] ) && memchr ( t[0], '-', tl[0] ) ) {      /* DOS */
        int dir = ( tl[2] == 5 ) && !g_ascii_strncasecmp ( t[2], "<DIR>", 5 );
        name = t[3];
        len = (size_t) ( end - name );
        if ( ( len == 1 && name[0] == '.' ) || ( len == 2 && name[0] == '.' && name[1] == '.' ) ) return;
        list_add ( r, name, len, dir, dir ? 0 : g_ascii_strtoull ( t[2], NULL, 10 ) );
        return;
    };
    if ( !strchr ( "-dlpbcs", t[0][0] ) ) return;
    for ( int i = 3; i + 2 < k; i++ ) {
        if ( is_month ( t[i], tl[i] ) && isdigit ( (unsigned char) t[i + 1][0] ) && isdigit ( (unsigned char) t[i + 2][0] ) ) {
            name = t[i + 2] + tl[i + 2];
            while ( ( name < end ) && ( *name == ' ' ) ) name++;
            len = (size_t) ( end - name );
            if ( t[0][0] == 'l' ) {
                for ( char *a = name; a + 4 <= end; a++ ) if ( !memcmp ( a, " -> ", 4 ) ) {
                        len = (size_t) ( a - name );
                        break;
                    };
            };
            if ( ( len == 1 && name[0] == '.' ) || ( len == 2 && name[0] == '.' && name[1] == '.' ) ) return;
            list_add ( r, name, len, t[0][0] == 'd', g_ascii_strtoull ( t[i - 1], NULL, 10 ) );
            return;
        };
    };
}


/* ---------------- sockets ---------------- */

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


static void set_timeouts ( SOCKET s ) {
#ifdef _WIN32
    DWORD ms = FTP_TIMEOUT_MS;
    setsockopt ( s, SOL_SOCKET, SO_RCVTIMEO, (const char*) &ms, sizeof ( ms ) );
    setsockopt ( s, SOL_SOCKET, SO_SNDTIMEO, (const char*) &ms, sizeof ( ms ) );
#else
    struct timeval tv = { FTP_TIMEOUT_MS / 1000, 0 };
    setsockopt ( s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof ( tv ) );
    setsockopt ( s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof ( tv ) );
#endif
}


/* A connection to the address a (IPv4) with the time limit; INVALID_SOCKET on a failure. */
static SOCKET connect_addr ( const struct sockaddr_in *a ) {
    SOCKET s = socket ( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( s == INVALID_SOCKET ) return s;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket ( s, FIONBIO, &nb );
#else
    fcntl ( s, F_SETFL, fcntl ( s, F_GETFL, 0 ) | O_NONBLOCK );
#endif
    connect ( s, (const struct sockaddr*) a, sizeof ( *a ) );
    fd_set w, e;
    FD_ZERO ( &w );
    FD_ZERO ( &e );
    FD_SET ( s, &w );
    FD_SET ( s, &e );
    struct timeval tv = { FTP_TIMEOUT_MS / 1000, 0 };
    int ok = ( select ( (int) s + 1, NULL, &w, &e, &tv ) > 0 ) && FD_ISSET ( s, &w ) && !FD_ISSET ( s, &e );
    if ( ok ) {
        int err = 0;
#ifdef _WIN32
        int el = sizeof ( err );
#else
        socklen_t el = sizeof ( err );
#endif
        getsockopt ( s, SOL_SOCKET, SO_ERROR, (char*) &err, &el );
        ok = ( err == 0 );
    };
    if ( !ok ) {
        closesocket ( s );
        return INVALID_SOCKET;
    };
#ifdef _WIN32
    nb = 0;
    ioctlsocket ( s, FIONBIO, &nb );
#else
    fcntl ( s, F_SETFL, fcntl ( s, F_GETFL, 0 ) & ~O_NONBLOCK );
#endif
    set_timeouts ( s );
    return s;
}


static int send_all ( SOCKET s, const char *d, size_t n ) {
    while ( n ) {
        int k = send ( s, d, (int) ( n > 65536 ? 65536 : n ), 0 );
        if ( k <= 0 ) return 0;
        d += k;
        n -= (size_t) k;
    };
    return 1;
}


static int send_cmd ( st_FTPREQ *r, const char *cmd, const char *arg ) {
    char b[FTP_PATH + 16];
    int n = arg ? snprintf ( b, sizeof ( b ), "%s %s\r\n", cmd, arg ) : snprintf ( b, sizeof ( b ), "%s\r\n", cmd );
    if ( ( n < 0 ) || ( n >= (int) sizeof ( b ) ) ) return 0;
    return send_all ( r->cs, b, (size_t) n );
}


/* One line of the control connection into r->line. 1 = a line, 0 = closed, -1 = time limit or error. */
static int read_line ( st_FTPREQ *r ) {
    for (;; ) {
        char *nl = memchr ( r->rbuf, '\n', (size_t) r->rlen );
        if ( nl ) {
            size_t n = (size_t) ( nl - r->rbuf );
            size_t c = ( n < sizeof ( r->line ) - 1 ) ? n : sizeof ( r->line ) - 1;
            memcpy ( r->line, r->rbuf, c );
            if ( c && ( r->line[c - 1] == '\r' ) ) c--;
            r->line[c] = 0x00;
            r->rlen -= (int) ( n + 1 );
            memmove ( r->rbuf, nl + 1, (size_t) r->rlen );
            return 1;
        };
        if ( r->rlen >= (int) sizeof ( r->rbuf ) ) r->rlen = 0;    /* a line too long: dropped */
        int k = recv ( r->cs, &r->rbuf[r->rlen], (int) sizeof ( r->rbuf ) - r->rlen, 0 );
        if ( k == 0 ) return 0;
        if ( k < 0 ) return -1;
        r->rlen += k;
    };
}


/* A whole reply (multi-line too): its code, the last line in r->line; -CLOUD_ERR_* on an error. */
static int reply ( st_FTPREQ *r ) {
    int multi = 0;
    for (;; ) {
        int k = read_line ( r );
        if ( k == 0 ) return -CLOUD_ERR_FAILED;
        if ( k < 0 ) return -CLOUD_ERR_TIMEOUT;
        const char *l = r->line;
        if ( !isdigit ( (unsigned char) l[0] ) || !isdigit ( (unsigned char) l[1] ) || !isdigit ( (unsigned char) l[2] ) ) continue;
        int code = ( l[0] - '0' ) * 100 + ( l[1] - '0' ) * 10 + ( l[2] - '0' );
        if ( multi ) {
            if ( ( code == multi ) && ( l[3] != '-' ) ) return code;
            continue;
        };
        if ( l[3] == '-' ) {
            multi = code;
            continue;
        };
        return code;
    };
}


/* A command and its reply: the code, or -CLOUD_ERR_*. */
static int command ( st_FTPREQ *r, const char *cmd, const char *arg ) {
    if ( g_atomic_int_get ( &r->cancel ) ) return -CLOUD_ERR_FAILED;
    if ( !send_cmd ( r, cmd, arg ) ) return -CLOUD_ERR_FAILED;
    return reply ( r );
}


/* The data connection (EPSV, else PASV; to the address of the control connection). INVALID_SOCKET on an error,
 * *err its code. */
static SOCKET data_open ( st_FTPREQ *r, int *err ) {
    int port = 0;
    int c = command ( r, "EPSV", NULL );
    if ( c < 0 ) {
        *err = -c;
        return INVALID_SOCKET;
    };
    if ( c == 229 ) {
        const char *p = strstr ( r->line, "|||" );
        port = p ? atoi ( p + 3 ) : 0;
    } else {
        c = command ( r, "PASV", NULL );
        if ( c < 0 ) {
            *err = -c;
            return INVALID_SOCKET;
        };
        if ( c != 227 ) {
            *err = CLOUD_ERR_FAILED;
            return INVALID_SOCKET;
        };
        const char *p = strchr ( r->line, '(' );
        int v[6];
        if ( p && ( sscanf ( p + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5] ) == 6 ) )
            port = ( v[4] << 8 ) | v[5];
    };
    if ( ( port <= 0 ) || ( port > 65535 ) ) {
        *err = CLOUD_ERR_INVALID;
        return INVALID_SOCKET;
    };
    struct sockaddr_in a;
#ifdef _WIN32
    int al = sizeof ( a );
#else
    socklen_t al = sizeof ( a );
#endif
    getpeername ( r->cs, (struct sockaddr*) &a, &al );
    a.sin_port = htons ( (uint16_t) port );
    SOCKET d = connect_addr ( &a );
    if ( d == INVALID_SOCKET ) *err = CLOUD_ERR_FAILED;
    return d;
}


/* ---------------- the request (its thread) ---------------- */

static int login ( st_FTPREQ *r ) {
    if ( !net_init ( ) ) return CLOUD_ERR_NOT_CONNECTED;
    struct addrinfo hints, *res = NULL;
    memset ( &hints, 0, sizeof ( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf ( port, sizeof ( port ), "%d", r->srv.port );
    if ( getaddrinfo ( r->srv.host, port, &hints, &res ) || !res ) return CLOUD_ERR_NOT_CONNECTED;
    struct sockaddr_in a;
    memcpy ( &a, res->ai_addr, sizeof ( a ) );
    freeaddrinfo ( res );
    r->cs = connect_addr ( &a );
    if ( r->cs == INVALID_SOCKET ) return CLOUD_ERR_NOT_CONNECTED;
    int c = reply ( r );
    if ( c < 0 ) return -c;
    if ( c != 220 ) return CLOUD_ERR_NOT_CONNECTED;
    c = command ( r, "USER", r->srv.user );
    if ( ( c == 331 ) || ( c == 332 ) ) c = command ( r, "PASS", r->srv.pass );
    if ( c < 0 ) return -c;
    if ( c == 530 ) return UNICARD_FTP_ERR_LOGIN;
    if ( ( c != 230 ) && ( c != 202 ) ) return CLOUD_ERR_FAILED;
    c = command ( r, "TYPE", "I" );
    if ( c < 0 ) return -c;
    return ( c / 100 == 2 ) ? 0 : CLOUD_ERR_FAILED;
}


/* LIST, RETR, STOR over the data connection. */
static int transfer ( st_FTPREQ *r ) {
    int err = 0;
    if ( OP_GET == r->op ) {
        int c = command ( r, "SIZE", r->path );
        if ( c < 0 ) return -c;
        if ( c == 213 ) r->total = g_ascii_strtoull ( &r->line[4], NULL, 10 );
    };
    SOCKET d = data_open ( r, &err );
    if ( d == INVALID_SOCKET ) return err;
    const char *cmd = ( OP_LIST == r->op ) ? "LIST" : ( OP_GET == r->op ) ? "RETR" : "STOR";
    int c = command ( r, cmd, r->path );
    if ( c < 0 ) {
        closesocket ( d );
        return -c;
    };
    if ( ( c != 150 ) && ( c != 125 ) ) {
        closesocket ( d );
        return CLOUD_ERR_FAILED;
    };
    FILE *f = NULL;
    if ( OP_GET == r->op ) f = g_fopen ( r->local, "wb" );
    if ( OP_PUT == r->op ) f = g_fopen ( r->local, "rb" );
    if ( ( OP_LIST != r->op ) && !f ) {
        closesocket ( d );
        return UNICARD_FTP_ERR_FS;
    };
    char b[16384], line[512];
    size_t ll = 0;
    int res = 0;
    if ( OP_PUT == r->op ) {
        size_t n;
        while ( !res && ( n = fread ( b, 1, sizeof ( b ), f ) ) > 0 ) {
            if ( g_atomic_int_get ( &r->cancel ) ) res = CLOUD_ERR_FAILED;
            else if ( !send_all ( d, b, n ) ) res = CLOUD_ERR_FAILED;
            else r->moved += n;
        };
        if ( !res && ferror ( f ) ) res = UNICARD_FTP_ERR_FS;
#ifdef _WIN32
        shutdown ( d, SD_SEND );
#else
        shutdown ( d, SHUT_WR );
#endif
    } else {
        for (;; ) {
            if ( g_atomic_int_get ( &r->cancel ) ) {
                res = CLOUD_ERR_FAILED;
                break;
            };
            int k = recv ( d, b, sizeof ( b ), 0 );
            if ( k == 0 ) break;
            if ( k < 0 ) {
                res = CLOUD_ERR_TIMEOUT;
                break;
            };
            if ( OP_GET == r->op ) {
                if ( fwrite ( b, 1, (size_t) k, f ) != (size_t) k ) {
                    res = UNICARD_FTP_ERR_FS;
                    break;
                };
                r->moved += (guint64) k;
            } else {
                for ( int i = 0; i < k; i++ ) {
                    if ( b[i] == '\n' ) {
                        if ( ll && ( line[ll - 1] == '\r' ) ) ll--;
                        list_line ( r, line, ll );
                        ll = 0;
                    } else if ( ll < sizeof ( line ) ) {
                        line[ll++] = b[i];
                    };
                };
            };
        };
        if ( ll ) list_line ( r, line, ll );            /* the last line without its end */
    };
    closesocket ( d );
    if ( f && ( fclose ( f ) != 0 ) && !res && ( OP_GET == r->op ) ) res = UNICARD_FTP_ERR_FS;
    if ( res ) return res;
    c = reply ( r );
    if ( c < 0 ) return -c;
    if ( ( c != 226 ) && ( c != 250 ) ) return CLOUD_ERR_FAILED;
    if ( ( OP_GET == r->op ) && r->total && ( r->moved != r->total ) ) return CLOUD_ERR_INVALID;
    if ( ( OP_LIST == r->op ) && r->list_full ) return CLOUD_ERR_TOO_LARGE;
    return 0;
}


static int simple ( st_FTPREQ *r ) {
    int c;
    if ( UNICARD_FTP_REN == r->what ) {
        c = command ( r, "RNFR", r->path );
        if ( c < 0 ) return -c;
        if ( c != 350 ) return CLOUD_ERR_FAILED;
        c = command ( r, "RNTO", r->path2 );
    } else if ( UNICARD_FTP_MKD == r->what ) {
        c = command ( r, "MKD", r->path );
    } else {
        c = command ( r, "DELE", r->path );
        if ( c >= 500 ) c = command ( r, "RMD", r->path );     /* a directory? */
    };
    if ( c < 0 ) return -c;
    return ( c / 100 == 2 ) ? 0 : CLOUD_ERR_FAILED;
}


static gpointer ftp_thread ( gpointer data ) {
    st_FTPREQ *r = data;
    r->cs = INVALID_SOCKET;
    int res = login ( r );
    if ( !res ) res = ( OP_CMD == r->op ) ? simple ( r ) : transfer ( r );
    if ( r->cs != INVALID_SOCKET ) {
        if ( !res ) send_cmd ( r, "QUIT", NULL );
        closesocket ( r->cs );
    };
    if ( g_atomic_int_get ( &r->cancel ) && !res ) res = CLOUD_ERR_FAILED;
    if ( res && ( OP_GET == r->op ) && r->local ) g_remove ( r->local );    /* an unfinished download away */
    g_atomic_int_set ( &r->result, res );
    g_atomic_int_set ( &r->done, 1 );
    return NULL;
}


static void req_free ( st_FTPREQ *r ) {
    if ( !r ) return;
    if ( r->thread ) g_thread_join ( r->thread );
    g_free ( r->local );
    g_free ( r->list );
    g_free ( r );
}


/* A new request: BUSY while one runs; NULL with *err on a bad path or an unknown server. */
static st_FTPREQ* req_new ( int op, const char *path, int *err ) {
    if ( s_zombie && g_atomic_int_get ( &s_zombie->done ) ) {
        req_free ( s_zombie );
        s_zombie = NULL;
    };
    if ( s_req || s_zombie ) {
        *err = CLOUD_ERR_BUSY;
        return NULL;
    };
    char name[FTP_NAME];
    st_FTPREQ *r = g_new0 ( st_FTPREQ, 1 );
    r->op = op;
    if ( !split ( path, name, r->path ) ) {
        g_free ( r );
        *err = CLOUD_ERR_INVALID;
        return NULL;
    };
    if ( !cfg_find ( name, &r->srv ) ) {
        g_free ( r );
        *err = CLOUD_ERR_FAILED;            /* no such server in ftp.cfg */
        return NULL;
    };
    return r;
}


static int req_start ( st_FTPREQ *r ) {
    s_req = r;
    r->thread = g_thread_new ( "unicard-ftp", ftp_thread, r );
    return 0;
}


int unicard_ftpc_list ( const char *path ) {
    int err = 0;
    st_FTPREQ *r = req_new ( OP_LIST, path, &err );
    if ( !r ) return err;
    r->list = g_malloc ( LIST_MAX );
    return req_start ( r );
}


int unicard_ftpc_get ( const char *path, char *dst_host ) {
    int err = 0;
    st_FTPREQ *r = req_new ( OP_GET, path, &err );
    if ( !r ) {
        g_free ( dst_host );
        return err;
    };
    r->local = dst_host;
    return req_start ( r );
}


int unicard_ftpc_put ( char *src_host, const char *path ) {
    int err = 0;
    st_FTPREQ *r = req_new ( OP_PUT, path, &err );
    if ( !r ) {
        g_free ( src_host );
        return err;
    };
    GStatBuf st;
    r->local = src_host;
    r->total = ( g_stat ( src_host, &st ) == 0 ) ? (guint64) st.st_size : 0;
    return req_start ( r );
}


int unicard_ftpc_cmd ( int what, const char *path, const char *path2 ) {
    int err = 0;
    st_FTPREQ *r = req_new ( OP_CMD, path, &err );
    if ( !r ) return err;
    r->what = what;
    if ( UNICARD_FTP_REN == what ) {
        char n1[FTP_NAME], n2[FTP_NAME], tmp[FTP_PATH];
        if ( !path2 || g_ascii_strncasecmp ( path2, "ftp:", 4 ) || !split ( path, n1, tmp ) ||
             !split ( path2, n2, r->path2 ) || g_ascii_strcasecmp ( n1, n2 ) ) {
            g_free ( r );
            return CLOUD_ERR_INVALID;       /* RNTO only on the same server */
        };
    };
    return req_start ( r );
}


int unicard_ftpc_result ( char *list, size_t max, int *len ) {
    st_FTPREQ *r = s_req;
    *len = 0;
    if ( !r ) return CLOUD_ERR_FAILED;
    if ( !g_atomic_int_get ( &r->done ) ) return -1;
    int res = g_atomic_int_get ( &r->result );
    if ( ( OP_LIST == r->op ) && !res && list ) {
        size_t n = ( (size_t) r->list_len < max ) ? (size_t) r->list_len : max;
        memcpy ( list, r->list, n );
        *len = (int) n;
    };
    s_req = NULL;
    req_free ( r );
    return res;
}


uint8_t unicard_ftpc_percent ( void ) {
    st_FTPREQ *r = s_req;
    if ( !r || !r->total ) return 0;
    guint64 p = r->moved * 100 / r->total;
    return ( p > 99 ) ? 99 : (uint8_t) p;
}


void unicard_ftpc_cancel ( void ) {
    st_FTPREQ *r = s_req;
    if ( !r ) return;
    s_req = NULL;
    g_atomic_int_set ( &r->cancel, 1 );
    if ( g_atomic_int_get ( &r->done ) ) {
        req_free ( r );
        return;
    };
    if ( s_zombie ) req_free ( s_zombie );      /* (a thread ends within the time limit) */
    s_zombie = r;
}
