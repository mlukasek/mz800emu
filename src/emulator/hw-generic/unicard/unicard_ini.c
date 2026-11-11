/*
 * File:   unicard_ini.c
 * Author: Martin Lukasek
 *
 * Created on 11. listopadu 2026
 *
 * Konfigurační soubory karty (INI) pro GETCONFIG a SETCONFIG (UC API rev 2).
 * Pravidla čtení podle firmwaru mk3b (main.c: INI_filecfg_radek, is_alphanum,
 * is_comment). Specifikace: UC-UniMgr docs/api.md.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include <string.h>

#include "unicard_ini.h"


typedef enum en_INI_LINE {
    INI_LINE_OTHER = 0,     /* prázdný řádek, komentář, nesrozumitelný řádek */
    INI_LINE_SECTION,       /* [JMÉNO] */
    INI_LINE_BAD_SECTION,   /* '[' bez platného jména: firmware pak ignoruje vše do další sekce */
    INI_LINE_KEY            /* KLÍČ [= hodnota] */
} en_INI_LINE;


static int ini_alnum ( char c ) {
    return g_ascii_isalnum ( c ) || ( c == '_' );
}


static int ini_comment ( char c ) {
    return ( c == '#' ) || ( c == ';' );
}


static int ini_space ( char c ) {
    return (unsigned char) c <= ' ';
}


/* Rozbor jednoho řádku (bez konce řádku) mezi s a end. Jméno sekce/klíče
 * velkými písmeny do name, hodnota do value. */
static en_INI_LINE ini_parse_line ( const char *s, const char *end, char *name, size_t name_size,
                                    char *value, size_t value_size ) {
    size_t n = 0;
    name[0] = 0x00;
    value[0] = 0x00;
    while ( ( s < end ) && ini_space ( *s ) ) s++;
    if ( ( s < end ) && ( *s == '[' ) ) {
        s++;
        while ( ( s < end ) && ini_space ( *s ) ) s++;
        if ( !( ( s < end ) && ini_alnum ( *s ) ) ) return INI_LINE_BAD_SECTION;
        while ( ( s < end ) && ini_alnum ( *s ) ) {
            if ( n + 1 < name_size ) name[n++] = g_ascii_toupper ( *s );
            s++;
        };
        name[n] = 0x00;
        while ( ( s < end ) && ini_space ( *s ) ) s++;
        return ( ( s < end ) && ( *s == ']' ) ) ? INI_LINE_SECTION : INI_LINE_BAD_SECTION;
    };
    if ( ( s >= end ) || ini_comment ( *s ) ) return INI_LINE_OTHER;
    while ( ( s < end ) && ini_alnum ( *s ) ) {
        if ( n + 1 < name_size ) name[n++] = g_ascii_toupper ( *s );
        s++;
    };
    name[n] = 0x00;
    if ( n == 0 ) return INI_LINE_OTHER;
    while ( ( s < end ) && ini_space ( *s ) ) s++;
    if ( ( s >= end ) || ini_comment ( *s ) ) return INI_LINE_KEY;     /* klíč bez hodnoty */
    if ( *s != '=' ) return INI_LINE_OTHER;
    s++;
    while ( ( s < end ) && ini_space ( *s ) ) s++;
    const char *v = s;
    while ( ( s < end ) && !ini_comment ( *s ) ) s++;
    const char *ve = s;
    while ( ( ve > v ) && ini_space ( ve[-1] ) ) ve--;
    size_t vl = (size_t) ( ve - v );
    if ( vl >= value_size ) vl = value_size - 1;
    memcpy ( value, v, vl );
    value[vl] = 0x00;
    return INI_LINE_KEY;
}


/* Další řádek od p: konec řádku bez CR/LF do *lend, začátek dalšího vrací. */
static const char* ini_next_line ( const char *p, const char *end, const char **lend ) {
    const char *le = memchr ( p, '\n', (size_t) ( end - p ) );
    if ( !le ) le = end;
    const char *e = le;
    if ( ( e > p ) && ( e[-1] == '\r' ) ) e--;
    *lend = e;
    return ( le < end ) ? le + 1 : end;
}


void unicard_ini_read_section ( const char *data, gsize len, const char *section,
                                const char *initial_section, GPtrArray *out ) {
    char cur[64];
    g_strlcpy ( cur, initial_section ? initial_section : "", sizeof ( cur ) );
    const char *p = data;
    const char *end = data + len;
    while ( p < end ) {
        const char *lend;
        const char *next = ini_next_line ( p, end, &lend );
        char name[64], value[256];
        switch ( ini_parse_line ( p, lend, name, sizeof ( name ), value, sizeof ( value ) ) ) {
            case INI_LINE_SECTION:
                g_strlcpy ( cur, name, sizeof ( cur ) );
                break;
            case INI_LINE_BAD_SECTION:
                cur[0] = 0x00;
                break;
            case INI_LINE_KEY:
                if ( 0 == strcmp ( cur, section ) ) {
                    char *rec = g_malloc0 ( UNICARD_INI_REC_SIZE );
                    g_strlcpy ( rec, name, UNICARD_INI_KEY_SIZE );
                    g_strlcpy ( rec + UNICARD_INI_KEY_SIZE, value, UNICARD_INI_VALUE_SIZE );
                    g_ptr_array_add ( out, rec );
                };
                break;
            default:
                break;
        };
        p = next;
    };
}


/* Připojí "KLÍČ=hodnota" a konec řádku; předtím dokončí řádek, pokud
 * výstup nekončí koncem řádku. */
static void ini_append_key ( GString *o, const char *key, const char *value, const char *nl ) {
    if ( ( o->len > 0 ) && ( o->str[o->len - 1] != '\n' ) ) g_string_append ( o, nl );
    g_string_append_printf ( o, "%s=%s%s", key, value, nl );
}


char* unicard_ini_set ( const char *data, gsize len, const char *section, const char *initial_section,
                        const char *key, const char *value, gsize *out_len ) {
    const char *nl = memchr ( data, '\r', len ) ? "\r\n" : "\n";
    GString *o = g_string_sized_new ( len + 128 );
    char cur[64];
    g_strlcpy ( cur, initial_section ? initial_section : "", sizeof ( cur ) );
    int in_sec = ( 0 == strcmp ( cur, section ) );
    int seen_sec = in_sec;
    int done = 0;
    const char *p = data;
    const char *end = data + len;

    while ( p < end ) {
        const char *lend;
        const char *next = ini_next_line ( p, end, &lend );
        char name[64], val[256];
        en_INI_LINE t = ini_parse_line ( p, lend, name, sizeof ( name ), val, sizeof ( val ) );

        if ( ( t == INI_LINE_SECTION ) || ( t == INI_LINE_BAD_SECTION ) ) {
            /* Konec hledané sekce bez klíče: přidat ho na její konec. */
            if ( in_sec && !done && value[0] ) {
                ini_append_key ( o, key, value, nl );
                done = 1;
            };
            g_strlcpy ( cur, ( t == INI_LINE_SECTION ) ? name : "", sizeof ( cur ) );
            in_sec = ( 0 == strcmp ( cur, section ) );
            if ( in_sec ) seen_sec = 1;
        } else if ( ( t == INI_LINE_KEY ) && in_sec && !done && ( 0 == strcmp ( name, key ) ) ) {
            /* První řádek klíče: přepsat, prázdná hodnota ho smaže. */
            if ( value[0] ) ini_append_key ( o, key, value, nl );
            done = 1;
            p = next;
            continue;
        };
        g_string_append_len ( o, p, (gssize) ( next - p ) );
        p = next;
    };

    if ( !done && value[0] ) {
        if ( !seen_sec ) {
            if ( ( o->len > 0 ) && ( o->str[o->len - 1] != '\n' ) ) g_string_append ( o, nl );
            g_string_append_printf ( o, "[%s]%s", section, nl );
        };
        ini_append_key ( o, key, value, nl );
    };

    *out_len = o->len;
    return g_string_free ( o, FALSE );
}
