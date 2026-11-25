/*
 * unicard_ftp.h - the volume ftp: of the Unicard against real FTP servers (unicard_ftp.c).
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 */
#ifndef UNICARD_FTP_H
#define UNICARD_FTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define UNICARD_FTP_MKD     1
#define UNICARD_FTP_DELE    2       /* a directory: RMD */
#define UNICARD_FTP_REN     4

#define UNICARD_FTP_ERR_LOGIN   8   /* the server refused the login (530), as the firmware */
#define UNICARD_FTP_ERR_FS      0x80 /* a file of the SD card (the host) could not be written or read */

    /** @brief Has /unicard/ftp.cfg of the emulated SD card a server? */
    extern int unicard_ftpc_has_server ( void );

    /** @brief "ftp:/": the servers of ftp.cfg as FILELIST text (directories). The length, -1 = too long. */
    extern int unicard_ftpc_servers ( char *out, size_t max );

    /**
     * @brief Start a request in its own thread. 0 = it runs, else CLOUD_ERR_* (5 = a request still runs).
     *        get takes dst_host (the host's path of the file on the SD card, freed here), put src_host the same.
     */
    extern int unicard_ftpc_list ( const char *path );
    extern int unicard_ftpc_get ( const char *path, char *dst_host );
    extern int unicard_ftpc_put ( char *src_host, const char *path );
    extern int unicard_ftpc_cmd ( int what, const char *path, const char *path2 );

    /**
     * @brief The result of the request: -1 it runs, 0 done, else CLOUD_ERR_*, UNICARD_FTP_ERR_LOGIN or
     *        UNICARD_FTP_ERR_FS. A listing goes to list (*len bytes, at most max). After a result the module is free.
     */
    extern int unicard_ftpc_result ( char *list, size_t max, int *len );

    /** @brief The progress of a download or an upload, 0-99. */
    extern uint8_t unicard_ftpc_percent ( void );

    /** @brief STORNO, RESET: the request ends (a downloaded part is deleted). */
    extern void unicard_ftpc_cancel ( void );

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_FTP_H */
