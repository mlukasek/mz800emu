/*
 * File:   unicard_ini.h
 * Author: Martin Lukasek
 *
 * Created on 11. listopadu 2026
 *
 * Konfigurační soubory karty (INI) pro GETCONFIG a SETCONFIG (UC API rev 2).
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

#ifndef UNICARD_INI_H
#define UNICARD_INI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <glib.h>

    /* Záznam GETCONFIG: klíč 16 B + hodnota 64 B, obojí doplněné nulami. */
#define UNICARD_INI_KEY_SIZE    16
#define UNICARD_INI_VALUE_SIZE  64
#define UNICARD_INI_REC_SIZE    ( UNICARD_INI_KEY_SIZE + UNICARD_INI_VALUE_SIZE )

    /**
     * @brief Záznamy sekce INI tak, jak je čte firmware mk3b (main.c,
     *        INI_filecfg_radek).
     *
     * - mezery a řídicí znaky na začátku řádku se přeskočí;
     * - `[JMÉNO]` začíná sekci (písmena, číslice, `_`, převedeno na velká);
     * - řádek začínající `#` nebo `;` je komentář;
     * - `KLÍČ = hodnota` (klíč na velká písmena, hodnota do konce řádku nebo
     *   do komentáře, bez mezer na okrajích), `KLÍČ` sám = prázdná hodnota.
     *
     * @param data obsah souboru
     * @param len délka
     * @param section hledaná sekce (velkými písmeny)
     * @param initial_section sekce řádků před první hlavičkou (network.cfg
     *        má `NETWORK`), nebo NULL
     * @param out pole, do kterého se přidají záznamy (g_free každého), každý
     *        UNICARD_INI_REC_SIZE bajtů
     */
    extern void unicard_ini_read_section ( const char *data, gsize len, const char *section,
                                           const char *initial_section, GPtrArray *out );

    /**
     * @brief Nový obsah INI s nastaveným klíčem (SETCONFIG).
     *
     * Přepíše první řádek klíče v sekci, prázdná hodnota ho smaže. Chybějící
     * klíč přidá na konec sekce, chybějící sekci na konec souboru. Komentáře,
     * pořadí a ostatní sekce zůstanou. Konce řádků jako v souboru (CR LF, když
     * je v něm aspoň jeden).
     *
     * @return nově alokovaný obsah (g_free), délka v @p out_len
     */
    extern char* unicard_ini_set ( const char *data, gsize len, const char *section, const char *initial_section,
                                   const char *key, const char *value, gsize *out_len );

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_INI_H */
